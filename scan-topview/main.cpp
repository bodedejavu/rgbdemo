/**
 * Copyright (C) 2013 ManCTL SARL <contact@manctl.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Nicolas Burrus <nicolas.burrus@manctl.com>
 */

#include <ntk/ntk.h>
#include <ntk/camera/calibration.h>

#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <cstdlib>
#include <sstream>
#include <iomanip>

#include <ntk/image/sift_gpu.h>
#include <ntk/camera/opencv_grabber.h>
#include <ntk/camera/file_grabber.h>
#include <ntk/camera/rgbd_frame_recorder.h>
#include <ntk/camera/rgbd_grabber_factory.h>
#include <ntk/mesh/mesh_generator.h>
#include <ntk/mesh/surfels_rgbd_modeler.h>
#include <ntk/mesh/table_object_rgbd_modeler.h>
#include <ntk/geometry/incremental_pose_estimator.h>

#include "GuiController.h"
#include "ModelAcquisitionController.h"

#include <QApplication>
#include <QMetaType>
#include <QMessageBox>

using namespace ntk;

namespace opt
{
ntk::arg<int> debug_level("--debug", "Debug level", 1);
ntk::arg<const char*> dir_prefix("--prefix", "Directory prefix for output", "grab1");
ntk::arg<int> first_index("--istart", "First image index", 0);
ntk::arg<const char*> calibration_file("--calibration", "Calibration file (yml)", 0);
ntk::arg<const char*> image("--image", "Fake mode, use given still image", 0);
ntk::arg<const char*> directory("--directory", "Fake mode, use all view???? images in dir.", 0);
ntk::arg<int> camera_id("--camera-id", "Camera id for opencv", 0);
ntk::arg<bool> pa10("--pa10", "WARNING: hold the emergency button -- Enable PA10 Controller", 0);
ntk::arg<bool> sync("--sync", "Synchronization mode", 0);
ntk::arg<const char*> pa10_controller("--pa10-controller", "Pa10 trajectory controller", "model");
ntk::arg<bool> openni("--openni", "Force OpenNI driver", 0);
ntk::arg<bool> freenect("--freenect", "Force freenect driver", 0);
ntk::arg<bool> kin4win("--kin4win", "Force kin4win driver", 0);
ntk::arg<bool> softkinetic("--softkinetic", "Force softkinetic driver", 0);
ntk::arg<bool> pmd("--pmd", "Force pmd nano driver", 0);
ntk::arg<bool> use_icp("--icp", "use ICP refinement", 0);
ntk::arg<bool> high_resolution("--highres", "High resolution color image.", 0);
ntk::arg<const char*> pose_estimator("--pose-estimator",
                                     "Relative pose estimator (file|delta|image)",
                                     "image");
ntk::arg<const char*> model_initial_pose("--pose-initial", "Model initial pose", 0);
ntk::arg<const char*> model_delta_pose("--pose-delta", "Model delta pose", 0);

}

int main (int argc, char** argv)
{
    arg_base::set_help_option("-h");
    arg_parse(argc, argv);
    ntk_debug_level = opt::debug_level();
    cv::setBreakOnError(true);

    QApplication app (argc, argv);

    RGBDGrabberFactory& grabber_factory = RGBDGrabberFactory::instance();
    RGBDGrabberFactory::Params params;

    if (opt::directory())
        params.directory = opt::directory();

    if (opt::openni())
        params.default_type = RGBDGrabberFactory::OPENNI;

    if (opt::freenect())
        params.default_type = RGBDGrabberFactory::FREENECT;

    if (opt::kin4win())
        params.default_type = RGBDGrabberFactory::KIN4WIN;

    if (opt::softkinetic())
        params.default_type = RGBDGrabberFactory::SOFTKINETIC;

    if (opt::pmd())
        params.default_type = RGBDGrabberFactory::PMD;

    if (opt::calibration_file())
        params.calibration_file = opt::calibration_file();

    if (opt::sync())
        params.synchronous = true;

    std::vector<RGBDGrabberFactory::GrabberData> grabbers;
    grabbers = grabber_factory.createGrabbers(params);

    if (grabbers.size() < 1)
    {
        QMessageBox::critical(0, "Fatal error",
                                  "Cannot connect to any RGBD device.\n\nPlease check that it is correctly plugged to the computer.");
        return 1;
    }

    RGBDGrabber* grabber = grabbers[0].grabber;
    RGBDProcessor* rgbd_processor = grabbers[0].processor;

    bool ok = grabber->connectToDevice();
    if (!ok)
    {
        ntk_dbg(0) << "WARNING: connectToDevice failed.";
        return 1;
    }

    rgbd_processor->setMaxNormalAngle(40);
    rgbd_processor->setFilterFlag(RGBDProcessorFlags::ComputeMapping, true);
    rgbd_processor->setFilterFlag(RGBDProcessorFlags::FilterThresholdDepth, true);
    rgbd_processor->setMinDepth(0.05f);
    rgbd_processor->setMaxDepth(1.5f);

    MeshGenerator* mesh_generator = 0;
    ntk::RGBDCalibrationPtr calib_data = grabber->calibrationData();

    if (calib_data)
    {
        mesh_generator = new MeshGenerator();
    }

    RGBDFrameRecorder frame_recorder (opt::dir_prefix());
    frame_recorder.setFrameIndex(opt::first_index());
    frame_recorder.setSaveOnlyRaw(true);
    frame_recorder.setUseBinaryRaw(true);

    GuiController gui_controller (*grabber, *rgbd_processor);
    grabber->addEventListener(&gui_controller);
    gui_controller.setFrameRecorder(frame_recorder);

    if (opt::sync())
        gui_controller.setPaused(true);

    TableObjectRGBDModeler modeler;
    modeler.setDepthFilling(true);
    modeler.setRemoveSmallStructures(true);

    ModelAcquisitionController* acq_controller = 0;
    acq_controller = new ModelAcquisitionController (gui_controller, modeler);
    acq_controller->setPaused(true);

    IncrementalPoseEstimatorFromImage* pose_estimator = new DummyIncrementalPoseEstimator();

    acq_controller->setPoseEstimator(pose_estimator);
    gui_controller.setModelAcquisitionController(*acq_controller);

    if (mesh_generator)
        gui_controller.setMeshGenerator(*mesh_generator);

    grabber->start();

    app.exec();
    delete mesh_generator;
    delete acq_controller;
}
