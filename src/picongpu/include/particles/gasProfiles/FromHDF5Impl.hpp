/**
 * Copyright 2013-2016 Felix Schmitt, Rene Widera
 *
 * This file is part of PIConGPU.
 *
 * PIConGPU is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * PIConGPU is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with PIConGPU.
 * If not, see <http://www.gnu.org/licenses/>.
 */


#pragma once

#include "simulation_defines.hpp"
#include "memory/buffers/GridBuffer.hpp"
#include "memory/boxes/DataBoxDim1Access.hpp"
#include "simulationControl/MovingWindow.hpp"
#include "fields/Fields.hpp"
#include "dataManagement/DataConnector.hpp"

#include <splash/splash.h>

namespace picongpu
{

namespace gasProfiles
{

template<typename T_ParamClass>
struct FromHDF5Impl : public T_ParamClass
{
    typedef T_ParamClass ParamClass;

    template<typename T_SpeciesType>
    struct apply
    {
        typedef FromHDF5Impl<ParamClass> type;
    };

    HINLINE FromHDF5Impl(uint32_t currentStep)
    {
        const uint32_t numSlides = MovingWindow::getInstance( ).getSlideCounter( currentStep );
        PMACC_AUTO(window, MovingWindow::getInstance().getWindow(currentStep));
        loadHDF5(window);
        const SubGrid<simDim>& subGrid = Environment<simDim>::get().SubGrid();
        DataSpace<simDim> localCells = subGrid.getLocalDomain( ).size;
        totalGpuOffset = subGrid.getLocalDomain( ).offset;
        totalGpuOffset.y( ) += numSlides * localCells.y( );
    }

    /** Calculate the gas density from HDF5 file
     *
     * @param totalCellOffset total offset including all slides [in cells]
     */
    HDINLINE float_X operator()(const DataSpace<simDim>& totalCellOffset)
    {
        const DataSpace<simDim> localCellIdx(totalCellOffset - totalGpuOffset);
        return precisionCast<float_X>(deviceDataBox(localCellIdx + SuperCellSize::toRT()*int(GUARD_SIZE)).x());
    }

private:

    void loadHDF5(Window &window)
    {
        using namespace splash;
        DataConnector &dc = Environment<>::get().DataConnector();
        FieldTmp& fieldTmp = dc.getData<FieldTmp > (FieldTmp::getName(), true);
        PMACC_AUTO(&fieldBuffer, fieldTmp.getGridBuffer());

        deviceDataBox = fieldBuffer.getDeviceBuffer().getDataBox();

        GridController<simDim> &gc = Environment<simDim>::get().GridController();
        const PMacc::Selection<simDim>& localDomain = Environment<simDim>::get().SubGrid().getLocalDomain();
        const uint32_t numSlides = MovingWindow::getInstance().getSlideCounter(0);
        const uint32_t maxOpenFilesPerNode = 1;

        /* get a new ParallelDomainCollector for our MPI rank only*/
        ParallelDomainCollector pdc(
                                    MPI_COMM_SELF,
                                    gc.getCommunicator().getMPIInfo(),
                                    Dimensions(1, 1, 1),
                                    maxOpenFilesPerNode);

        try
        {
            /* setup ParallelDomainCollector pdc to read the density information from hdf5 */
            DataCollector::FileCreationAttr attr;
            DataCollector::initFileCreationAttr(attr);
            attr.fileAccType = DataCollector::FAT_READ;

            pdc.open(ParamClass::filename, attr);

            /* set which part of the hdf5 file our MPI rank reads */
            DataSpace<simDim> globalSlideOffset;
            globalSlideOffset.y() = numSlides * localDomain.size.y();

            Dimensions domainOffset(0, 0, 0);
            for (uint32_t d = 0; d < simDim; ++d)
                domainOffset[d] = localDomain.offset[d] + globalSlideOffset[d];

            if (gc.getPosition().y() == 0)
                domainOffset[1] += window.globalDimensions.offset.y();

            DataSpace<simDim> localDomainSize = localDomain.size;
            Dimensions domainSize(1, 1, 1);
            for (uint32_t d = 0; d < simDim; ++d)
                domainSize[d] = localDomainSize[d];

            /* clear host buffer with default value */
            fieldBuffer.getHostBuffer().setValue(float1_X(ParamClass::defaultDensity));

            /* get dimensions and offsets (collective call) */
            Domain fileDomain = pdc.getGlobalDomain(ParamClass::iteration, ParamClass::datasetName);
            Dimensions fileDomainEnd = fileDomain.getOffset() + fileDomain.getSize();
            DataSpace<simDim> accessSpace;
            DataSpace<simDim> accessOffset;

            Dimensions fileAccessSpace(1, 1, 1);
            Dimensions fileAccessOffset(0, 0, 0);

            /* For each dimension, compute how file domain and local simulation domain overlap
             * and which sizes and offsets are required for loading data from the file.
             **/
            for (uint32_t d = 0; d < simDim; ++d)
            {
                /* file domain in/in-after sim domain */
                if (fileDomain.getOffset()[d] >= domainOffset[d] &&
                    fileDomain.getOffset()[d] <= domainOffset[d] + domainSize[d])
                {
                    accessSpace[d] = std::min(domainOffset[d] + domainSize[d] - fileDomain.getOffset()[d],
                                              fileDomain.getSize()[d]);
                    fileAccessSpace[d] = accessSpace[d];

                    accessOffset[d] = fileDomain.getOffset()[d] - domainOffset[d];
                    fileAccessOffset[d] = 0;
                    continue;
                }

                /* file domain before-in sim domain */
                if (fileDomainEnd[d] >= domainOffset[d] &&
                    fileDomainEnd[d] <= domainOffset[d] + domainSize[d])
                {
                    accessSpace[d] = fileDomainEnd[d] - domainOffset[d];
                    fileAccessSpace[d] = accessSpace[d];

                    accessOffset[d] = 0;
                    fileAccessOffset[d] = domainOffset[d] - fileDomain.getOffset()[d];
                    continue;
                }

                /* sim domain in file domain */
                if (domainOffset[d] >= fileDomain.getOffset()[d] &&
                    domainOffset[d] + domainSize[d] <= fileDomainEnd[d])
                {
                    accessSpace[d] = domainSize[d];
                    fileAccessSpace[d] = accessSpace[d];

                    accessOffset[d] = 0;
                    fileAccessOffset[d] = domainOffset[d] - fileDomain.getOffset()[d];
                    continue;
                }

                /* file domain and sim domain do not intersect, do not load anything */
                accessSpace[d] = 0;
                break;
            }

            /* allocate temporary buffer for hdf5 data */
            typedef typename FieldTmp::ValueType::type ValueType;
            ValueType *tmpBfr = NULL;

            size_t accessSize = accessSpace.productOfComponents();
            if (accessSize > 0)
            {
                tmpBfr = new ValueType[accessSize];

                Dimensions sizeRead(0, 0, 0);
                pdc.read(
                         ParamClass::iteration,
                         fileAccessSpace,
                         fileAccessOffset,
                         ParamClass::datasetName,
                         sizeRead,
                         tmpBfr);

                if (sizeRead.getScalarSize() != accessSize)
                {
                    __delete(tmpBfr);
                    return;
                }

                /* get the databox of the host buffer */
                PMACC_AUTO(dataBox, fieldBuffer.getHostBuffer().getDataBox());
                /* get a 1D access object to the databox */
                typedef DataBoxDim1Access< typename FieldTmp::DataBoxType > D1Box;
                DataSpace<simDim> guards = fieldBuffer.getGridLayout().getGuard();
                D1Box d1RAccess(dataBox.shift(guards + accessOffset), accessSpace);

                /* copy from temporary buffer to fieldTmp host buffer */
                for (int i = 0; i < accessSpace.productOfComponents(); ++i)
                {
                    d1RAccess[i].x() = tmpBfr[i];
                }

                __delete(tmpBfr);
            }

            pdc.close();

            /* copy host data to the device */
            fieldBuffer.hostToDevice();
            __getTransactionEvent().waitForFinished();

        }
        catch (const DCException& e)
        {
            std::cerr << e.what() << std::endl;
            return;
        }

        return;
    }

    PMACC_ALIGN(deviceDataBox,FieldTmp::DataBoxType);
    PMACC_ALIGN(totalGpuOffset,DataSpace<simDim>);

};
} //namespace gasProfiles
} //namespace picongpu
