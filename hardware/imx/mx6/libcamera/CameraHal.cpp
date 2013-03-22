/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Copyright 2009-2012 Freescale Semiconductor, Inc.
 */


#include <cutils/properties.h>
#include "CameraHal.h"
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <hardware_legacy/power.h>
#include <ui/GraphicBufferMapper.h>
#include <ui/Rect.h>
#include "gralloc_priv.h"

namespace android {

    CameraHal::CameraHal(int cameraid)
        :
        mCaptureRunning(0),
        mExitCaptureThread(false),
        mPreviewRunning(0),
        mExitPreviewThread(false),
        mExitEncodeThread(false),
        mTakePictureInProcess(false),
        mParameters(),
        mCallbackCookie(NULL),
        mNotifyCb(NULL),
        mDataCb(NULL),
        mDataCbTimestamp(NULL),
        mCaptureFrameThread(NULL),
        mPreviewShowFrameThread(NULL),
        mEncodeFrameThread(NULL),
        mAutoFocusThread(NULL),
        mTakePicThread(NULL),
        mLock(),
        mSupportedPictureSizes(NULL),
        mSupportedPreviewSizes(NULL),
        mSupportedFPS(NULL),
        mSupprotedThumbnailSizes(NULL),
        mSupportPreviewFormat(NULL),
        mNativeWindow(NULL),
        mMsgEnabled(0),
        mPreviewMemory(NULL),
        mVideoBufNume(VIDEO_OUTPUT_BUFFER_NUM),
        mVideoMemory(NULL),
        mDefaultPreviewFormat(V4L2_PIX_FMT_NV12), //the optimized selected format, hard code
        mPreviewFrameSize(0),
        mTakePicFlag(false),
        mUvcSpecialCaptureFormat(V4L2_PIX_FMT_YUYV),
        mCaptureFrameSize(0),
        mCaptureBufNum(0),
        mEnqueuedBufs(0),
        mRecordRunning(0),
        mCurrentRecordFrame(0),
        nCameraBuffersQueued(0),
        mPreviewHeapBufNum(PREVIEW_HEAP_BUF_NUM),
        mTakePicBufQueNum(TAKE_PIC_QUE_BUF_NUM),
        mCameraReady(false),
        mCaptureDeviceOpen(false),
        mIsCaptureBufsAllocated(0),
        mPowerLock(false),
        mDirectInput(false),
        mCameraid(cameraid),
        mPreviewRotate(CAMERA_PREVIEW_BACK_REF)
   {
        CAMERA_LOG_FUNC;
        preInit();
    }

    CameraHal :: ~CameraHal()
    {
        CAMERA_LOG_FUNC;
        CameraMiscDeInit();
        CloseCaptureDevice();
        FreeInterBuf();
        postDestroy();
        if(mVideoMemory != NULL) {
            mVideoMemory->release(mVideoMemory);
        }
        if(mPreviewMemory != NULL) {
            mPreviewMemory->release(mPreviewMemory);
        }
    }

    void CameraHal :: release()
    {
        CAMERA_LOG_FUNC;
        Mutex::Autolock lock(mLock);

        mCameraReady = false;
        CameraHALStopPreview();
        UnLockWakeLock();
        return;
    }

    void CameraHal :: preInit()
    {
        CAMERA_LOG_FUNC;
        mVpuSupportFmt[0] = v4l2_fourcc('N','V','1','2');
        mVpuSupportFmt[1] = v4l2_fourcc('Y','U','1','2');
    }
    void CameraHal :: postDestroy()
    {
        CAMERA_LOG_FUNC;
    }

    CAMERA_HAL_RET CameraHal :: setCaptureDevice(sp<CaptureDeviceInterface> capturedevice)
    {
        CAMERA_LOG_FUNC;
        CAMERA_HAL_RET ret = CAMERA_HAL_ERR_NONE;
        if (mCameraReady == false)
            mCaptureDevice = capturedevice;
        else
            ret = CAMERA_HAL_ERR_BAD_ALREADY_RUN;
        return ret;
    }

    CAMERA_HAL_RET CameraHal :: setJpegEncoder(sp<JpegEncoderInterface>jpegencoder)
    {
        CAMERA_LOG_FUNC;
        CAMERA_HAL_RET ret = CAMERA_HAL_ERR_NONE;
        if (mCameraReady == false)
            mJpegEncoder = jpegencoder;
        else
            ret = CAMERA_HAL_ERR_BAD_ALREADY_RUN;
        return ret;
    }

    CAMERA_HAL_RET CameraHal::Init()
    {
        CAMERA_LOG_FUNC;
        CAMERA_HAL_RET ret = CAMERA_HAL_ERR_NONE;
        mCameraReady == true;
        mCaptureDevice->GetDevType(&mSensorType);

        if ((ret = AllocInterBuf())<0)
            return ret;
        if ((ret = InitCameraHalParam()) < 0)
            return ret;
        if ((ret = CameraMiscInit()) < 0)
            return ret;

        return ret;
    }
    void  CameraHal::setPreviewRotate(CAMERA_PREVIEW_ROTATE previewRotate)
    {
        CAMERA_LOG_FUNC;
        mPreviewRotate = previewRotate;
        return ;
    }

    CAMERA_HAL_RET  CameraHal :: AllocInterBuf()
    {
        CAMERA_LOG_FUNC;
        CAMERA_HAL_RET ret = CAMERA_HAL_ERR_NONE;

        mSupportedPictureSizes = (char *)malloc(CAMER_PARAM_BUFFER_SIZE);
        mSupportedPreviewSizes = (char *)malloc(CAMER_PARAM_BUFFER_SIZE);
        mSupportedFPS          = (char *)malloc(CAMER_PARAM_BUFFER_SIZE);
        mSupprotedThumbnailSizes = (char *)malloc(CAMER_PARAM_BUFFER_SIZE);
        mSupportPreviewFormat = (char *)malloc(CAMER_PARAM_BUFFER_SIZE);

        if (mSupportedPictureSizes == NULL ||
                mSupportedPreviewSizes == NULL ||
                mSupportedFPS          == NULL ||
                mSupprotedThumbnailSizes == NULL ||
                mSupportPreviewFormat == NULL)
            ret = CAMERA_HAL_ERR_ALLOC_BUF;

        return ret;
    }
    void  CameraHal :: FreeInterBuf()
    {
        CAMERA_LOG_FUNC;
        if (mSupportedPictureSizes)
            free(mSupportedPictureSizes);
        if (mSupportedPreviewSizes)
            free(mSupportedPreviewSizes);
        if (mSupportedFPS)
            free(mSupportedFPS);
        if (mSupprotedThumbnailSizes)
            free(mSupprotedThumbnailSizes);
    }

    CAMERA_HAL_RET CameraHal :: InitCameraHalParam()
    {
        CAMERA_LOG_FUNC;
        CAMERA_HAL_RET ret = CAMERA_HAL_ERR_NONE;

        if ((ret = InitCameraBaseParam(&mParameters)) < 0)
            return ret;

        if ((ret = InitPictureExifParam(&mParameters)) < 0)
            return ret;

        return ret;
    }

    CAMERA_HAL_RET CameraHal::CameraMiscInit()
    {
        CAMERA_LOG_FUNC;
        CAMERA_HAL_RET ret = CAMERA_HAL_ERR_NONE;

        mPreviewRunning = false;
        mCaptureRunning = false;
        mWaitForTakingPicture = false;
        sem_init(&mCaptureStoppedCondition, 0, 0);
        sem_init(&mPreviewStoppedCondition, 0, 0);
        //sem_init(&mEncodeStoppedCondition, 0, 0);
        sem_init(&mTakingPicture, 0, 0);
        //mPostProcessRunning = false;
        //mEncodeRunning = false;
        mCaptureFrameThread = new CaptureFrameThread(this);
        mPreviewShowFrameThread = new PreviewShowFrameThread(this);
        mEncodeFrameThread = new EncodeFrameThread(this);
        mTakePicThread= new TakePicThread(this);

        if (mCaptureFrameThread == NULL || mPreviewShowFrameThread == NULL ||
                mEncodeFrameThread == NULL || mTakePicThread == NULL){
            return CAMERA_HAL_ERR_INIT;
        }
        return ret;
    }
    CAMERA_HAL_RET CameraHal::CameraMiscDeInit()
    {
        CAMERA_LOG_FUNC;
        CAMERA_HAL_RET ret = CAMERA_HAL_ERR_NONE;
        mCaptureThreadQueue.postQuitMessage();
        //Make sure all thread been exit, in case they still
        //access the message queue
        if(mCaptureFrameThread != NULL)
            mCaptureFrameThread->requestExitAndWait();

        //Post Quite message to make sure the thread can be exited
        //In case mCaptureFrameThread not been started yet in CTS test
        mPreviewThreadQueue.postQuitMessage();
        if(mPreviewShowFrameThread != NULL)
            mPreviewShowFrameThread->requestExitAndWait();
        mEncodeThreadQueue.postQuitMessage();
        if(mEncodeFrameThread != NULL)
            mEncodeFrameThread->requestExitAndWait();
        if(mTakePicThread != NULL)
            mTakePicThread->requestExitAndWait();
        return ret;
    }

    CAMERA_HAL_RET CameraHal::InitCameraPreviewFormatToParam(int nFmt)
    {
        CAMERA_LOG_FUNC;
        int i, n;
        unsigned int uFormat[MAX_QUERY_FMT_TIMES];
        char fmtStr[40];

        memset(fmtStr, 0, 40);
        convertPreviewFormatToString(fmtStr, 40, mPreviewCapturedFormat);
        mParameters.setPreviewFormat(fmtStr);
        mParameters.set(CameraParameters::KEY_VIDEO_FRAME_FORMAT, fmtStr);

        memset(uFormat, 0, sizeof(uFormat));
        for(n = 0; n < nFmt; n++) {
            uFormat[n] = mSensorSupportFmt[n];
        }

        memset(fmtStr, 0, 40);
        for(i = 0; i < n; i++) {
            for (int j = 0; j < MAX_VPU_SUPPORT_FORMAT; j++) {
                //should report VPU support format.
                if(uFormat[i] == mVpuSupportFmt[j]) {
                    if(uFormat[i] == v4l2_fourcc('Y','U','1','2')) {
                        strcat(fmtStr, "yuv420p");
                        strcat(fmtStr, ",");
                    }
                    else if(uFormat[i] == v4l2_fourcc('N','V','1','2')) {
                        strcat(fmtStr, "yuv420sp");
                        strcat(fmtStr, ",");
                    }
                    //else if(mSensorSupportFmt[i] == v4l2_fourcc('Y','U','Y','V')) {
                    //    strcat(fmtStr, "yuv422i-yuyv");
                    //    strcat(fmtStr, ",");
                    //}
                }
            }
        }

        mParameters.set(CameraParameters::KEY_SUPPORTED_PREVIEW_FORMATS, fmtStr);
        return CAMERA_HAL_ERR_NONE;
    }

    CAMERA_HAL_RET CameraHal :: InitCameraBaseParam(CameraParameters *pParam)
    {
        CAMERA_LOG_FUNC;
        char TmpStr[20];
        unsigned int CapPreviewFmt[MAX_QUERY_FMT_TIMES];
        struct capture_config_t CaptureSizeFps;
        int  previewCnt= 0, pictureCnt = 0, i;
        char previewFmt[20] = {0};

        //the Camera Open here will not be close immediately, for later preview.
        if (OpenCaptureDevice() < 0)
            return CAMERA_HAL_ERR_OPEN_CAPTURE_DEVICE;

        memset(mSensorSupportFmt, 0, sizeof(unsigned int)*MAX_QUERY_FMT_TIMES);

        for(i =0; i< MAX_QUERY_FMT_TIMES; i ++){
            if (mCaptureDevice->EnumDevParam(OUTPU_FMT,&(mSensorSupportFmt[i])) < 0)
                break;
        }
        if (i == 0)
            return CAMERA_HAL_ERR_GET_PARAM;

        //InitCameraPreviewFormatToParam(i);

        if (NegotiateCaptureFmt(false) < 0)
            return CAMERA_HAL_ERR_GET_PARAM;

        //mCaptureDeviceCfg.fmt stores format related to driver.
        CaptureSizeFps.fmt = mCaptureDeviceCfg.fmt;//mPreviewCapturedFormat;

        //mPreviewCapturedFormat stores format report to app.
        InitCameraPreviewFormatToParam(i);
        //memset(TmpStr, 0, 20);
        //mPreviewCapturedFormat stores format report to app.
        //convertPreviewFormatToString(TmpStr, 20, mPreviewCapturedFormat);
        //convertPreviewFormatToString(TmpStr, 20, mCaptureDeviceCfg.fmt);
        //mParameters.setPreviewFormat(TmpStr);

        CAMERA_LOG_INFO("mCaptureDeviceCfg.fmt is %x", mCaptureDeviceCfg.fmt);

        for(;;){
            if (mCaptureDevice->EnumDevParam(FRAME_SIZE_FPS,&CaptureSizeFps) <0){
                CAMERA_LOG_RUNTIME("get the frame size and time interval error");
                break;
            }
            memset(TmpStr, 0, 20);
            sprintf(TmpStr, "%dx%d", CaptureSizeFps.width,CaptureSizeFps.height);
            CAMERA_LOG_INFO("Size: %s , Framerate: %d supported", TmpStr, (CaptureSizeFps.tv.denominator/CaptureSizeFps.tv.numerator));
            if (pictureCnt == 0)
                strncpy((char*) mSupportedPictureSizes, TmpStr, CAMER_PARAM_BUFFER_SIZE);
            else{
                strncat(mSupportedPictureSizes,  PARAMS_DELIMITER, CAMER_PARAM_BUFFER_SIZE);
                strncat(mSupportedPictureSizes, TmpStr, CAMER_PARAM_BUFFER_SIZE);
            }
            pictureCnt ++;

            //Limite the FPS and resolution for preview setting
            //Typically only max to 1080p for mipi, 720p for csi,
            //and minum 15 fps need for preview.
            unsigned int max_preview_w, max_preview_h;
            char device_name[CAMERA_SENSOR_LENGTH];
            memset(device_name, 0, CAMERA_SENSOR_LENGTH);
            mCaptureDevice->GetDevName(device_name);
            if(strstr(device_name, "mipi")) {
                max_preview_w = MAX_MIPI_PREVIEW_W;
                max_preview_h = MAX_MIPI_PREVIEW_H;
            } else {
                max_preview_w = MAX_CSI_PREVIEW_W;
                max_preview_h = MAX_CSI_PREVIEW_H;
            }

            if ((CaptureSizeFps.tv.denominator/CaptureSizeFps.tv.numerator >= 15)&&
                (CaptureSizeFps.width <= max_preview_w)&&(CaptureSizeFps.height <= max_preview_h)){
                if (previewCnt == 0)
                    strncpy((char*) mSupportedPreviewSizes, TmpStr, CAMER_PARAM_BUFFER_SIZE);
                else{
                    strncat(mSupportedPreviewSizes,  PARAMS_DELIMITER, CAMER_PARAM_BUFFER_SIZE);
                    strncat(mSupportedPreviewSizes, TmpStr, CAMER_PARAM_BUFFER_SIZE);
                }
                previewCnt ++;
            }
        }

        /*hard code here*/
        strcpy(mSupportedFPS, "15,30");
        CAMERA_LOG_INFO("SupportedPictureSizes is %s", mSupportedPictureSizes);
        CAMERA_LOG_INFO("SupportedPreviewSizes is %s", mSupportedPreviewSizes);
        CAMERA_LOG_INFO("SupportedFPS is %s", mSupportedFPS);

        pParam->set(CameraParameters::KEY_SUPPORTED_PICTURE_SIZES, mSupportedPictureSizes);
        pParam->set(CameraParameters::KEY_SUPPORTED_PREVIEW_SIZES, mSupportedPreviewSizes);
        pParam->set(CameraParameters::KEY_SUPPORTED_PREVIEW_FRAME_RATES, mSupportedFPS);
        pParam->set(CameraParameters::KEY_SUPPORTED_PREVIEW_FPS_RANGE, "(12000,17000),(25000,33000)");
        pParam->set(CameraParameters::KEY_PREVIEW_FPS_RANGE, "25000,33000");

        pParam->setPreviewSize(DEFAULT_PREVIEW_W, DEFAULT_PREVIEW_H);
        pParam->setPictureSize(DEFAULT_PICTURE_W, DEFAULT_PICTURE_H);
        pParam->setPreviewFrameRate(DEFAULT_PREVIEW_FPS);

        return CAMERA_HAL_ERR_NONE;

    }

    status_t CameraHal :: OpenCaptureDevice()
    {
        CAMERA_LOG_FUNC;
        status_t ret = NO_ERROR;
        if (mCaptureDeviceOpen){
            CAMERA_LOG_INFO("The capture device already open");
            return NO_ERROR;
        }
        else if (mCaptureDevice != NULL){
            if ( mCaptureDevice->DevOpen(mCameraid)<0 )
                return INVALID_OPERATION;
            mCaptureDeviceOpen = true;
        }else{
            CAMERA_LOG_ERR("no capture device assigned");
            return INVALID_OPERATION;
        }
        return ret;
    }
    void CameraHal ::CloseCaptureDevice()
    {
        CAMERA_LOG_FUNC;
        if (mCaptureDeviceOpen && mCaptureDevice != NULL){
            mCaptureDevice->DevClose();
            mCaptureDeviceOpen = false;
        }
    }

    CAMERA_HAL_RET CameraHal :: InitPictureExifParam(CameraParameters *pParam)
    {
        CAMERA_LOG_FUNC;
        char tmpBuffer[CAMER_PARAM_BUFFER_SIZE];

        /*hard code here*/
        pParam->set(CameraParameters::KEY_FOCUS_DISTANCES, "24.0,50.0,2147483648.0");
        pParam->setPictureFormat(CameraParameters::PIXEL_FORMAT_JPEG);
        pParam->set(CameraParameters::KEY_SUPPORTED_PICTURE_FORMATS, CameraParameters::PIXEL_FORMAT_JPEG);
        pParam->set(CameraParameters::KEY_JPEG_QUALITY, 100);
        strcpy(mSupprotedThumbnailSizes, "0x0,128x128,96x96");
        pParam->set(CameraParameters::KEY_SUPPORTED_JPEG_THUMBNAIL_SIZES, mSupprotedThumbnailSizes);
        pParam->set(CameraParameters::KEY_JPEG_THUMBNAIL_WIDTH, "96");
        pParam->set(CameraParameters::KEY_JPEG_THUMBNAIL_HEIGHT, "96");
        pParam->set(CameraParameters::KEY_JPEG_THUMBNAIL_QUALITY, "90");

        memset(tmpBuffer, '\0', sizeof(*tmpBuffer));
        strncat((char*) tmpBuffer, (const char*) CameraParameters::WHITE_BALANCE_AUTO, CAMER_PARAM_BUFFER_SIZE);
        strncat((char*) tmpBuffer, (const char*) PARAMS_DELIMITER, CAMER_PARAM_BUFFER_SIZE);
        strncat((char*) tmpBuffer, (const char*) CameraParameters::WHITE_BALANCE_INCANDESCENT, CAMER_PARAM_BUFFER_SIZE);
        strncat((char*) tmpBuffer, (const char*) PARAMS_DELIMITER, CAMER_PARAM_BUFFER_SIZE);
        strncat((char*) tmpBuffer, (const char*) CameraParameters::WHITE_BALANCE_FLUORESCENT, CAMER_PARAM_BUFFER_SIZE);
        strncat((char*) tmpBuffer, (const char*) PARAMS_DELIMITER, CAMER_PARAM_BUFFER_SIZE);
        strncat((char*) tmpBuffer, (const char*) CameraParameters::WHITE_BALANCE_DAYLIGHT, CAMER_PARAM_BUFFER_SIZE);
        strncat((char*) tmpBuffer, (const char*) PARAMS_DELIMITER, CAMER_PARAM_BUFFER_SIZE);
        strncat((char*) tmpBuffer, (const char*) CameraParameters::WHITE_BALANCE_SHADE, CAMER_PARAM_BUFFER_SIZE);
        pParam->set(CameraParameters::KEY_SUPPORTED_WHITE_BALANCE, tmpBuffer);
        pParam->set(CameraParameters::KEY_WHITE_BALANCE, CameraParameters::WHITE_BALANCE_AUTO);

        memset(tmpBuffer, '\0', sizeof(*tmpBuffer));
        strncat((char*) tmpBuffer, (const char*) CameraParameters::EFFECT_NONE, CAMER_PARAM_BUFFER_SIZE);
        strncat((char*) tmpBuffer, (const char*) PARAMS_DELIMITER, CAMER_PARAM_BUFFER_SIZE);
        strncat((char*) tmpBuffer, (const char*) CameraParameters::EFFECT_MONO, CAMER_PARAM_BUFFER_SIZE);
        strncat((char*) tmpBuffer, (const char*) PARAMS_DELIMITER, CAMER_PARAM_BUFFER_SIZE);
        strncat((char*) tmpBuffer, (const char*) CameraParameters::EFFECT_NEGATIVE, CAMER_PARAM_BUFFER_SIZE);
        strncat((char*) tmpBuffer, (const char*) PARAMS_DELIMITER, CAMER_PARAM_BUFFER_SIZE);
        strncat((char*) tmpBuffer, (const char*) CameraParameters::EFFECT_SOLARIZE,  CAMER_PARAM_BUFFER_SIZE);
        strncat((char*) tmpBuffer, (const char*) PARAMS_DELIMITER, CAMER_PARAM_BUFFER_SIZE);
        strncat((char*) tmpBuffer, (const char*) CameraParameters::EFFECT_SEPIA, CAMER_PARAM_BUFFER_SIZE);
        pParam->set(CameraParameters::KEY_SUPPORTED_EFFECTS, tmpBuffer);
        pParam->set(CameraParameters::KEY_EFFECT, CameraParameters::EFFECT_NONE);

        memset(tmpBuffer, '\0', sizeof(*tmpBuffer));
        strncat((char*) tmpBuffer, (const char*) CameraParameters::SCENE_MODE_AUTO, CAMER_PARAM_BUFFER_SIZE);
        strncat((char*) tmpBuffer, (const char*) PARAMS_DELIMITER, CAMER_PARAM_BUFFER_SIZE);
        strncat((char*) tmpBuffer, (const char*) CameraParameters::SCENE_MODE_PORTRAIT, CAMER_PARAM_BUFFER_SIZE);
        strncat((char*) tmpBuffer, (const char*) PARAMS_DELIMITER, CAMER_PARAM_BUFFER_SIZE);
        strncat((char*) tmpBuffer, (const char*) CameraParameters::SCENE_MODE_LANDSCAPE, CAMER_PARAM_BUFFER_SIZE);
        strncat((char*) tmpBuffer, (const char*) PARAMS_DELIMITER, CAMER_PARAM_BUFFER_SIZE);
        strncat((char*) tmpBuffer, (const char*) CameraParameters::SCENE_MODE_SPORTS, CAMER_PARAM_BUFFER_SIZE);
        strncat((char*) tmpBuffer, (const char*) PARAMS_DELIMITER, CAMER_PARAM_BUFFER_SIZE);
        strncat((char*) tmpBuffer, (const char*) CameraParameters::SCENE_MODE_NIGHT_PORTRAIT, CAMER_PARAM_BUFFER_SIZE);
        strncat((char*) tmpBuffer, (const char*) PARAMS_DELIMITER, CAMER_PARAM_BUFFER_SIZE);
        strncat((char*) tmpBuffer, (const char*) CameraParameters::SCENE_MODE_FIREWORKS, CAMER_PARAM_BUFFER_SIZE);
        strncat((char*) tmpBuffer, (const char*) PARAMS_DELIMITER, CAMER_PARAM_BUFFER_SIZE);
        strncat((char*) tmpBuffer, (const char*) CameraParameters::SCENE_MODE_NIGHT, CAMER_PARAM_BUFFER_SIZE);
        pParam->set(CameraParameters::KEY_SUPPORTED_SCENE_MODES, tmpBuffer);
        pParam->set(CameraParameters::KEY_SCENE_MODE, CameraParameters::SCENE_MODE_AUTO);

        pParam->set(CameraParameters::KEY_SUPPORTED_FOCUS_MODES, CameraParameters::FOCUS_MODE_AUTO);
        pParam->set(CameraParameters::KEY_FOCUS_MODE, CameraParameters::FOCUS_MODE_AUTO);

        pParam->set(CameraParameters::KEY_FOCAL_LENGTH, "10.001");
        pParam->set(CameraParameters::KEY_HORIZONTAL_VIEW_ANGLE, "54.8");
        pParam->set(CameraParameters::KEY_VERTICAL_VIEW_ANGLE, "42.5");
        pParam->set(CameraParameters::KEY_EXPOSURE_COMPENSATION, "0");
        pParam->set(CameraParameters::KEY_MAX_EXPOSURE_COMPENSATION, "0");
        pParam->set(CameraParameters::KEY_MIN_EXPOSURE_COMPENSATION, "0");
        pParam->set(CameraParameters::KEY_EXPOSURE_COMPENSATION_STEP, "0.0");

        memset(tmpBuffer, '\0', sizeof(*tmpBuffer));
        strncat((char*) tmpBuffer, (const char*) CameraParameters::ANTIBANDING_50HZ, CAMER_PARAM_BUFFER_SIZE);
        strncat((char*) tmpBuffer, (const char*) PARAMS_DELIMITER, CAMER_PARAM_BUFFER_SIZE);
        strncat((char*) tmpBuffer, (const char*) CameraParameters::ANTIBANDING_60HZ, CAMER_PARAM_BUFFER_SIZE);
        strncat((char*) tmpBuffer, (const char*) PARAMS_DELIMITER, CAMER_PARAM_BUFFER_SIZE);
        strncat((char*) tmpBuffer, (const char*) CameraParameters::ANTIBANDING_OFF, CAMER_PARAM_BUFFER_SIZE);
        pParam->set(CameraParameters::KEY_SUPPORTED_ANTIBANDING, tmpBuffer);
        pParam->set(CameraParameters::KEY_ANTIBANDING, CameraParameters::ANTIBANDING_OFF);

        memset(tmpBuffer, '\0', sizeof(*tmpBuffer));
        strncat( (char*) tmpBuffer, (const char*) CameraParameters::FLASH_MODE_OFF, CAMER_PARAM_BUFFER_SIZE);
        pParam->set(CameraParameters::KEY_SUPPORTED_FLASH_MODES, tmpBuffer);
        pParam->set(CameraParameters::KEY_FLASH_MODE, CameraParameters::FLASH_MODE_OFF);
        pParam->set(CameraParameters::KEY_ZOOM_SUPPORTED, CameraParameters::TRUE);
        pParam->set(CameraParameters::KEY_MAX_ZOOM, "1");
        // default zoom should be 0 as CTS defined
        pParam->set(CameraParameters::KEY_ZOOM, "0");
        //the zoom ratios in 1/100 increments. Ex: a zoom of 3.2x is
        //returned as 320. The number of elements is {@link
        //#getMaxZoom} + 1. The list is sorted from small to large. The
        //first element is always 100. The last element is the zoom
        //ratio of the maximum zoom value.
        pParam->set(CameraParameters::KEY_ZOOM_RATIOS, "100,200");

        return CAMERA_HAL_ERR_NONE;
    }

    sp<IMemoryHeap> CameraHal::getRawHeap() const
    {
        return NULL;
    }

    status_t CameraHal::dump(int fd) const
    {
        return NO_ERROR;
    }

    status_t CameraHal::sendCommand(int32_t command, int32_t arg1,
            int32_t arg2)
    {
        return BAD_VALUE;
    }

    void CameraHal::setCallbacks(camera_notify_callback notify_cb,
            camera_data_callback data_cb,
            camera_data_timestamp_callback data_cb_timestamp,
            camera_request_memory get_memory,
            void* user)
    {
        Mutex::Autolock lock(mLock);
        mNotifyCb = notify_cb;
        mDataCb = data_cb;
        mDataCbTimestamp = data_cb_timestamp;
        mRequestMemory = get_memory;
        mCallbackCookie = user;
    }

    void CameraHal::enableMsgType(int32_t msgType)
    {
        Mutex::Autolock lock(mLock);
        CAMERA_LOG_INFO("enableMsgType 0x%x", msgType);
        mMsgEnabled |= msgType;
    }

    void CameraHal::disableMsgType(int32_t msgType)
    {
        Mutex::Autolock lock(mLock);
        CAMERA_LOG_INFO("disableMsgType 0x%x", msgType);
        mMsgEnabled &= ~msgType;
    }
    bool CameraHal::msgTypeEnabled(int32_t msgType)
    {
        Mutex::Autolock lock(mLock);
        return (mMsgEnabled & msgType);
    }

    void CameraHal::putParameters(char *params)
    {
        free(params);
    }

    char* CameraHal::getParameters() const
    {
        CAMERA_LOG_FUNC;

        Mutex::Autolock lock(mLock);
        char* params_string;
        String8 params_str8;
        CameraParameters mParams = mParameters;

        params_str8 = mParams.flatten();
        params_string = (char*)malloc(sizeof(char) * (params_str8.length() + 1));
        strcpy(params_string, params_str8.string());
        return params_string;
    }

    status_t  CameraHal:: setParameters(const char* params)
    {
        CAMERA_LOG_FUNC;
        CameraParameters parameters;
        String8 str_params(params);

        parameters.unflatten(str_params);
        return setParameters(parameters);
    }

    status_t  CameraHal:: setParameters(const CameraParameters& params)
    {
        CAMERA_LOG_FUNC;
        int w, h;
        int framerate;
        int max_zoom,zoom, max_fps, min_fps;
        char tmp[128];
        Mutex::Autolock lock(mLock);

        max_zoom = params.getInt(CameraParameters::KEY_MAX_ZOOM);
        zoom = params.getInt(CameraParameters::KEY_ZOOM);
        if(zoom > max_zoom){
            CAMERA_LOG_ERR("Invalid zoom setting, zoom %d, max zoom %d",zoom,max_zoom);
            return BAD_VALUE;
        }
        if (!((strcmp(params.getPreviewFormat(), "yuv420sp") == 0) ||
                (strcmp(params.getPreviewFormat(), "yuv420p") == 0)// || (strcmp(params.getPreviewFormat(), "yuv422i-yuyv") == 0)
                )) {
            CAMERA_LOG_ERR("Only yuv420sp or yuv420pis supported, but input format is %s", params.getPreviewFormat());
            return BAD_VALUE;
        }

        if (strcmp(params.getPictureFormat(), "jpeg") != 0) {
            CAMERA_LOG_ERR("Only jpeg still pictures are supported");
            return BAD_VALUE;
        }

        params.getPreviewSize(&w, &h);
        sprintf(tmp, "%dx%d", w, h);
        CAMERA_LOG_INFO("Set preview size: %s", tmp);
        if (strstr(mSupportedPreviewSizes, tmp) == NULL){
            CAMERA_LOG_ERR("The preview size w %d, h %d is not corrected", w, h);
            return BAD_VALUE;
        }

        params.getPictureSize(&w, &h);
        sprintf(tmp, "%dx%d", w, h);
        CAMERA_LOG_INFO("Set picture size: %s", tmp);
        if (strstr(mSupportedPictureSizes, tmp) == NULL){
            CAMERA_LOG_ERR("The picture size w %d, h %d is not corrected", w, h);
            return BAD_VALUE;
        }

        framerate = params.getPreviewFrameRate();
        CAMERA_LOG_INFO("Set frame rate:%d FPS", framerate);
        if ((framerate > 30) || (framerate < 0) ){
            CAMERA_LOG_ERR("The framerate is not corrected");
            return BAD_VALUE;
        }

        params.getPreviewFpsRange(&min_fps, &max_fps);
        CAMERA_LOG_INFO("FPS range: %d - %d",min_fps, max_fps);
        if (max_fps < 1000 || min_fps < 1000 || max_fps > 33000 || min_fps > 33000){
            CAMERA_LOG_ERR("The fps range from %d to %d is error", min_fps, max_fps);
            return BAD_VALUE;
        }

        const char *pFlashStr;
        pFlashStr = params.get(CameraParameters::KEY_FLASH_MODE);
        if (strcmp(pFlashStr, CameraParameters::FLASH_MODE_OFF) != 0 && strcmp(pFlashStr, CameraParameters::FLASH_MODE_AUTO) != 0
                && strcmp(pFlashStr, CameraParameters::FLASH_MODE_ON) != 0 && strcmp(pFlashStr, CameraParameters::FLASH_MODE_RED_EYE) != 0
                && strcmp(pFlashStr, CameraParameters::FLASH_MODE_TORCH) != 0) {
            CAMERA_LOG_ERR("The flash mode is not corrected");
            return BAD_VALUE;
        }

        const char *pFocusStr;
        pFocusStr = params.get(CameraParameters::KEY_FOCUS_MODE);
        if(strcmp(pFocusStr, CameraParameters::FOCUS_MODE_AUTO) != 0 && strcmp(pFocusStr, CameraParameters::FOCUS_MODE_INFINITY) != 0
                && strcmp(pFocusStr, CameraParameters::FOCUS_MODE_MACRO) != 0 && strcmp(pFocusStr, CameraParameters::FOCUS_MODE_FIXED) != 0
                && strcmp(pFocusStr, CameraParameters::FOCUS_MODE_EDOF) != 0 && strcmp(pFocusStr, CameraParameters::FOCUS_MODE_CONTINUOUS_VIDEO) != 0) {
            CAMERA_LOG_ERR("The focus mode is not corrected");
            return BAD_VALUE;
        }
        mParameters = params;

        return NO_ERROR;
    }

    status_t CameraHal::setPreviewWindow(struct preview_stream_ops *window)
    {
        CAMERA_LOG_FUNC;
        if(window == NULL) {
            mIsCaptureBufsAllocated = 0;
            CAMERA_LOG_INFO("PreviewWindow is null");
        }
        else {
            CAMERA_LOG_ERR("PreviewWindow is valid");
        }
        mNativeWindow = window;
        if((mNativeWindow != NULL) && !mIsCaptureBufsAllocated && mCaptureBufNum) {
            if(PrepareCaptureBufs() < 0) {
                CAMERA_LOG_ERR("PrepareCaptureBufs() error");
                return BAD_VALUE;
            }
            if(CameraHALPreviewStart() < 0) {
                CAMERA_LOG_ERR("CameraHALPreviewStart() error");
                return BAD_VALUE;
            }
        }

        return NO_ERROR;
    }

    status_t CameraHal::freeBuffersToNativeWindow()
    {
        CAMERA_LOG_FUNC;

        if (mNativeWindow == NULL){
            CAMERA_LOG_ERR("the native window is null!");
            return BAD_VALUE;
        }

        GraphicBufferMapper &mapper = GraphicBufferMapper::get();
        buffer_handle_t *handle;
        for(unsigned int i = 0; i < mCaptureBufNum; i++) {
            if(mCaptureBuffers[i].buf_state == WINDOW_BUFS_DEQUEUED) {
                handle = (buffer_handle_t *)mCaptureBuffers[i].native_buf;
                if(handle != NULL) {
                    if(mCaptureBuffers[i].virt_start != NULL) {
                        mapper.unlock(*handle);
                    }
                    mNativeWindow->cancel_buffer(mNativeWindow, handle);
                }
            }
            else
                continue;
            mCaptureBuffers[i].buf_state = WINDOW_BUFS_INVALID;//WINDOW_BUFS_QUEUED;
            mCaptureBuffers[i].refCount = 0;
            mCaptureBuffers[i].native_buf = NULL;
            mCaptureBuffers[i].virt_start = NULL;
            mCaptureBuffers[i].length = 0;
            mCaptureBuffers[i].phy_offset = 0;
        }
        mCaptureBufNum = 0;

        return NO_ERROR;
    }

    int CameraHal::convertPreviewFormatToPixelFormat(unsigned int format)
    {
        int nFormat = 0;
        switch(format) {
            case v4l2_fourcc('N','V','1','2'):
                nFormat = HAL_PIXEL_FORMAT_YCbCr_420_SP;
                break;
            case v4l2_fourcc('Y','U','1','2'):
                nFormat = HAL_PIXEL_FORMAT_YCbCr_420_P;
                break;
            case v4l2_fourcc('Y','U','Y','V'):
                nFormat = HAL_PIXEL_FORMAT_YCbCr_422_I;
                break;
            default:
                CAMERA_LOG_ERR("Error: format not supported!");
                break;
        }
        CAMERA_LOG_INFO("pixel format: 0x%x", nFormat);
        return nFormat;
    }

    status_t CameraHal::allocateBuffersFromNativeWindow()
    {
        CAMERA_LOG_FUNC;

        status_t err;
        if (mNativeWindow == NULL){
            CAMERA_LOG_ERR("the native window is null!");
            return NO_ERROR;//BAD_VALUE;
        }

        //Make sure the buffer be physical continuous
        err = mNativeWindow->set_usage(mNativeWindow,
                GRALLOC_USAGE_SW_READ_NEVER | GRALLOC_USAGE_FORCE_CONTIGUOUS | GRALLOC_USAGE_HW_TEXTURE);
        if(err != 0){
            CAMERA_LOG_ERR("native_window_set_usage failed:%s(%d)",
                    strerror(-err), -err);
            return err;
        }

        //should use mPreviewCapturedFormat here.
        int uFormat = convertPreviewFormatToPixelFormat(mPreviewCapturedFormat);
        err = mNativeWindow->set_buffers_geometry(mNativeWindow,
                mCaptureDeviceCfg.width, mCaptureDeviceCfg.height,
                uFormat);
        if(err != 0){
            CAMERA_LOG_ERR("native_window_set_buffers_geometry failed:%s(%d)",
                    strerror(-err), -err);
            return err;
        }

        int minUndequeueBufs = 0;
        err = mNativeWindow->get_min_undequeued_buffer_count(mNativeWindow,
                &minUndequeueBufs);
        if(err != 0) {
            CAMERA_LOG_ERR("NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS query failed:%s(%d)",
                    strerror(-err), -err);
            return err;
        }

        if(mCaptureBufNum > PREVIEW_CAPTURE_BUFFER_NUM) {
            CAMERA_LOG_ERR("%s: the actual buffer number %d is too large than %d", __FUNCTION__, mCaptureBufNum, PREVIEW_CAPTURE_BUFFER_NUM);
            return BAD_VALUE;
        }

        err = mNativeWindow->set_buffer_count(mNativeWindow, mCaptureBufNum);
        if(err != 0) {
            CAMERA_LOG_ERR("native_window_set_buffer_count failed:%s(%d)",
                    strerror(-err), -err);
            return err;
        }

        unsigned int i;
        Rect bounds(mCaptureDeviceCfg.width, mCaptureDeviceCfg.height);
        void *pVaddr = NULL;
        GraphicBufferMapper &mapper = GraphicBufferMapper::get();
        for(i = 0; i < mCaptureBufNum; i++) {
            buffer_handle_t* buf_h = NULL;
            pVaddr = NULL;
            int stride;
            err = mNativeWindow->dequeue_buffer(mNativeWindow, &buf_h, &stride);
            if((err != 0) || (buf_h == NULL)) {
                CAMERA_LOG_ERR("dequeueBuffer failed: %s(%d)", strerror(-err), -err);
                return BAD_VALUE;
            }
            private_handle_t *handle = (private_handle_t *)(*buf_h);
            mapper.lock(handle, GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN, bounds, &pVaddr);
            if((handle->phys == 0) || (handle->base == 0) || (handle->size == 0)) {
                 CAMERA_LOG_ERR("%s: dequeue invalide Buffer, phys=0x%lx, base=%x, size=%d", __FUNCTION__, handle->phys, handle->base, handle->size);
                 mNativeWindow->cancel_buffer(mNativeWindow, buf_h);
                 return BAD_VALUE;
            }

            mCaptureBuffers[i].virt_start = (unsigned char *)handle->base;
            mCaptureBuffers[i].phy_offset = handle->phys;
            //Calculate the buffer size, for GPU doesn't reply this value.
            mCaptureBuffers[i].length =  handle->size;//mCaptureFrameSize
            mCaptureBuffers[i].native_buf = (void *)buf_h;
            mCaptureBuffers[i].refCount = 0;
            mCaptureBuffers[i].buf_state = WINDOW_BUFS_DEQUEUED;
            CAMERA_LOG_RUNTIME("mCaptureBuffers[%d]-phys=%x, base=%p, size=%d", i, mCaptureBuffers[i].phy_offset, mCaptureBuffers[i].virt_start, mCaptureBuffers[i].length);
        }

        return NO_ERROR;
    }

    status_t CameraHal::startPreview()
    {
        CAMERA_LOG_FUNC;
        status_t ret = NO_ERROR;

        if(mTakePictureInProcess) {
            mWaitForTakingPicture = true;
            sem_wait(&mTakingPicture);
            mWaitForTakingPicture = false;
        }
        Mutex::Autolock lock(mLock);
        mEnqueuedBufs = 0;

        mPreviewLock.lock();
        if (mPreviewRunning) {
            // already running
            CAMERA_LOG_RUNTIME("%s : preview thread already running", __func__);
            mPreviewLock.unlock();
            return NO_ERROR;//INVALID_OPERATION;
        }

        if ((ret == CameraHALStartPreview())<0) {
            CAMERA_LOG_RUNTIME("%s : CameraHALStartPreview error", __func__);
            mPreviewLock.unlock();
            return ret;
        }

        mPreviewRunning = true;
        mPreviewLock.unlock();

        mCaptureLock.lock();
        if(mCaptureRunning) {
            CAMERA_LOG_ERR("%s : preview thread already running", __func__);
            mCaptureLock.unlock();
            return NO_ERROR;
        }
        mCaptureRunning = true;
        mCaptureLock.unlock();

        LockWakeLock();
        return ret;
    }

    void CameraHal::stopPreview()
    {
        CAMERA_LOG_FUNC;
        struct timeval af_time, be_time;
        Mutex::Autolock lock(mLock);
        CameraHALStopPreview();
        UnLockWakeLock();

    }

    bool CameraHal::previewEnabled()
    {
        CAMERA_LOG_FUNC;
        return mPreviewRunning;
    }

    //update buffer for direct input in video recorder
    status_t CameraHal::updateDirectInput(bool bDirect)
    {
        unsigned int i;
        if (bDirect == true) {
            if(mCaptureBufNum <= 0)
                CAMERA_LOG_INFO("mCaptureBuf not allocated yet, will register it later");

            for(i = 0 ; i < mCaptureBufNum; i ++) {
                mVideoBufferPhy[i].phy_offset = mCaptureBuffers[i].phy_offset;
                CAMERA_LOG_INFO("Camera HAL physic address: %x", mCaptureBuffers[i].phy_offset);
                mVideoBufferPhy[i].length = mCaptureBuffers[i].length;
                memcpy((unsigned char*)mVideoMemory->data + i*mPreviewFrameSize,
                        (void*)&mVideoBufferPhy[i], sizeof(VIDEOFRAME_BUFFER_PHY));
            }
        }

        return NO_ERROR;
    }

    status_t CameraHal::storeMetaDataInBuffers(bool enable)
    {
        CAMERA_LOG_FUNC;
        mDirectInput = enable;
        updateDirectInput(enable);
        return NO_ERROR;
    }

    status_t CameraHal::startRecording()
    {
        CAMERA_LOG_FUNC;
        status_t ret = NO_ERROR;
        unsigned int i;

        mEncodeLock.lock();
        if (mRecordRunning == true ) {
            CAMERA_LOG_INFO("%s: Recording is already existed\n", __FUNCTION__);
            mEncodeLock.unlock();
            return ret;
        }


        if (mDirectInput == true) {
            for(i = 0; i < mVideoBufNume; i++) {
                mVideoBufferUsing[i] = 0;
            }
        }

        mRecordRunning = true;
        mEncodeLock.unlock();

        return NO_ERROR;
    }

    void CameraHal::stopRecording()
    {
        CAMERA_LOG_FUNC;

        mEncodeLock.lock();
        if(mRecordRunning) {
            mRecordRunning = false;
            mEncodeThreadQueue.postMessage(new CMessage(CMESSAGE_TYPE_STOP, 0));
            //stopRecording() will holde mLock in camera service,
            //when encodeframeThread() is in the call back mDataCbTimestamp.
            //That call back will make a call of releaseRecordingFrame, which
            //also try to acquire the lock mLock. It will make a dead lock between the two thread
            //sem_wait(&mEncodeStoppedCondition);
            CAMERA_LOG_RUNTIME("---%s, after wait--", __FUNCTION__);
        }
        mEncodeLock.unlock();
    }

    void CameraHal::releaseRecordingFrame(const void* mem)
    {
        //CAMERA_LOG_FUNC;
        int index;

        index = ((size_t)mem - (size_t)mVideoMemory->data) / mPreviewFrameSize;
        mVideoBufferUsing[index] = 0;

        if (mDirectInput == true) {
            if(mCaptureBuffers[index].refCount == 0) {
                CAMERA_LOG_ERR("warning:%s about to release mCaptureBuffers[%d].refcount=%d-", __FUNCTION__, index, mCaptureBuffers[index].refCount);
                return;
            }
            putBufferCount(&mCaptureBuffers[index]);
        }
    }

    bool CameraHal::recordingEnabled()
    {
        CAMERA_LOG_FUNC;
        return (mPreviewRunning && mRecordRunning);
    }

    status_t CameraHal::autoFocus()
    {
        CAMERA_LOG_FUNC;

        Mutex::Autolock lock(mLock);

        if (mAutoFocusThread != NULL)
            mAutoFocusThread.clear();

        mAutoFocusThread = new AutoFocusThread(this);
        if (mAutoFocusThread == NULL)
            return UNKNOWN_ERROR;
        return NO_ERROR;
    }

    status_t CameraHal::cancelAutoFocus()
    {
        CAMERA_LOG_FUNC;

        return NO_ERROR;
    }

    status_t CameraHal::takePicture()
    {
        CAMERA_LOG_FUNC;
        Mutex::Autolock lock(mLock);

        if(mTakePictureInProcess) {
            CAMERA_LOG_ERR("%s: takePicture already in process", __FUNCTION__);
            return INVALID_OPERATION;
        }

        if(mTakePicThread->run("takepicThread", PRIORITY_URGENT_DISPLAY) != NO_ERROR) {
            CAMERA_LOG_ERR("%s: could't run take picture thread", __FUNCTION__);
            return INVALID_OPERATION;
        }
        mTakePictureInProcess = true;

        return NO_ERROR;
    }

    status_t CameraHal::cancelPicture()
    {
        CAMERA_LOG_FUNC;
        mTakePicThread->requestExitAndWait();

        return NO_ERROR;
    }


    int CameraHal::autoFocusThread()
    {
        CAMERA_LOG_FUNC;
        int FocusFlag = 0;

        if (mMsgEnabled & CAMERA_MSG_FOCUS)
            mNotifyCb(CAMERA_MSG_FOCUS, true, 0, mCallbackCookie);

        return UNKNOWN_ERROR; //exit the thread
    }

    int CameraHal::takepicThread()
    {
        CAMERA_LOG_FUNC;
        CAMERA_LOG_INFO("Start taking picture!");

        /* Stop preview, start picture capture, and then restart preview again for CSI camera*/
        CameraHALStopPreview();
        cameraHALTakePicture();
        mTakePictureInProcess = false;

        return UNKNOWN_ERROR;
    }

    int CameraHal :: cameraHALTakePicture()
    {
        CAMERA_LOG_FUNC;
        int ret = NO_ERROR;
        unsigned int DeQueBufIdx = 0;
        struct jpeg_encoding_conf JpegEncConf;
        DMA_BUFFER *Buf_input, Buf_output;
        camera_memory_t* JpegMemBase = NULL;
        camera_memory_t *RawMemBase = NULL;

        int  max_fps, min_fps;
        int actual_fps = 15;

        if (mJpegEncoder == NULL){
            CAMERA_LOG_ERR("the jpeg encoder is NULL");
            return BAD_VALUE;
        }
        mParameters.getPictureSize((int *)&(mCaptureDeviceCfg.width),(int *)&(mCaptureDeviceCfg.height));
        //Default setting is 15FPS
        mCaptureDeviceCfg.tv.numerator = 1;
        mCaptureDeviceCfg.tv.denominator = 15;
        mCaptureDevice->GetDevName(mCameraSensorName);
        if (strstr(mCameraSensorName, "uvc") == NULL){
            //according to google's doc getPreviewFrameRate & getPreviewFpsRange should support both.
            // so here just a walkaround, if the app set the FpsRange, will follow this FpsRange.
            if((strstr(mCameraSensorName, "mipi") == NULL)&&
                (mCaptureDeviceCfg.width == 1920)&&
                (mCaptureDeviceCfg.height == 1080)) {
                //Workaround for 1080p setting for csi camera, which only 15fps is valid
                mCaptureDeviceCfg.tv.denominator = 15;
            } else {
                mParameters.getPreviewFpsRange(&min_fps, &max_fps);
                if (max_fps < 1000 || min_fps < 1000 || max_fps > 33000 || min_fps > 33000){
                    if (mParameters.getPreviewFrameRate() >= 15){
                        mCaptureDeviceCfg.tv.denominator = mParameters.getPreviewFrameRate();
                        CAMERA_LOG_INFO("Set Capture Fps %d", mParameters.getPreviewFrameRate());
                    }
                }
                else{
                    CAMERA_LOG_INFO("Set Capture Fps Range %d - %d",min_fps, max_fps);
                    actual_fps = min_fps > 15000? 30:15;
                    mCaptureDeviceCfg.tv.denominator = actual_fps;
                }
            }
        }else{
                mCaptureDeviceCfg.tv.denominator = 15;
        }
        mCaptureBufNum = PICTURE_CAPTURE_BUFFER_NUM;
        mTakePicFlag = true;
        if ((ret = GetJpegEncoderParam()) < 0)
            return ret;
        if ((ret = NegotiateCaptureFmt(true)) < 0)
            return ret;

        if ((ret = PrepareCaptureDevices()) < 0)
            return ret;

        if ((ret = PrepareJpegEncoder()) < 0)
            return ret;

        JpegMemBase = mRequestMemory(-1, mCaptureFrameSize, 1, NULL);
        if (JpegMemBase == NULL || JpegMemBase->data == NULL){
            ret = NO_MEMORY;
            goto Pic_out;
        }

        if (mCaptureDevice->DevStart()<0){
            CAMERA_LOG_ERR("the capture start up failed !!!!");
            return INVALID_OPERATION;
        }

        for (unsigned int i =0;;){
            if (mCaptureDevice->DevDequeue(&DeQueBufIdx) < 0){
                LOGE("VIDIOC_DQBUF Failed!!!");
                ret = UNKNOWN_ERROR;
                goto Pic_out;
            }
            if (++i == mCaptureDeviceCfg.picture_waite_number)
                break;

            if (mCaptureDevice->DevQueue(DeQueBufIdx) < 0 ){
                ret = UNKNOWN_ERROR;
                goto Pic_out;
            }
        }

        Buf_input = &mCaptureBuffers[DeQueBufIdx];

        Buf_output.virt_start = (unsigned char *)(JpegMemBase->data);
        CAMERA_LOG_INFO("Generated a picture with mMsgEnabled 0x%x", mMsgEnabled);

        if (mMsgEnabled & CAMERA_MSG_SHUTTER) {
            CAMERA_LOG_INFO("CAMERA_MSG_SHUTTER");
            mNotifyCb(CAMERA_MSG_SHUTTER, 0, 0, mCallbackCookie);
        }

        if (mMsgEnabled & CAMERA_MSG_RAW_IMAGE) {
            CAMERA_LOG_INFO("CAMERA_MSG_RAW_IMAGE");
            RawMemBase = mRequestMemory(-1, mCaptureFrameSize, 1, NULL);

            if ( NULL == RawMemBase ) {
                CAMERA_LOG_INFO("Raw buffer allocation failed!");
                ret = UNKNOWN_ERROR;
                goto Pic_out;
            }
            void *dest = RawMemBase->data;

            if (NULL != dest) {
                void *src = &mCaptureBuffers[DeQueBufIdx];
                memcpy(dest, src, mCaptureFrameSize);
            }

            mDataCb(CAMERA_MSG_RAW_IMAGE, RawMemBase, 0, NULL, mCallbackCookie);

            RawMemBase->release(RawMemBase);
        }

        if ( mMsgEnabled & CAMERA_MSG_RAW_IMAGE_NOTIFY ) {
            CAMERA_LOG_INFO("CAMERA_MSG_RAW_IMAGE_NOTIFY");
            if(mNotifyCb)
                mNotifyCb(CAMERA_MSG_RAW_IMAGE_NOTIFY, 0, 0, mCallbackCookie);
        }

        if (mJpegEncoder->DoEncode(Buf_input,&Buf_output,&JpegEncConf) < 0){
            ret = UNKNOWN_ERROR;
            goto Pic_out;
        }

Pic_out:
        freeBuffersToNativeWindow();
        if ((JpegMemBase != NULL) &&(JpegMemBase->data != NULL) && (mMsgEnabled & CAMERA_MSG_COMPRESSED_IMAGE)) {
            CAMERA_LOG_INFO("==========CAMERA_MSG_COMPRESSED_IMAGE==================");
            mDataCb(CAMERA_MSG_COMPRESSED_IMAGE, JpegMemBase, 0, NULL, mCallbackCookie);
        }

        mCaptureDevice->DevStop();
        mCaptureDevice->DevDeAllocate();
        if(mSensorType == CAMERA_TYPE_UVC) {
            CloseCaptureDevice();
        }

        if(JpegMemBase) {
            JpegMemBase->release(JpegMemBase);
        }
        if(mWaitForTakingPicture) {
            sem_post(&mTakingPicture);
        }
        return ret;

    }

    int CameraHal :: GetJpegEncoderParam()
    {
        CAMERA_LOG_FUNC;
        int ret = NO_ERROR, i = 0;
        memset(mJpegEncoderSupportFmt, 0, sizeof(unsigned int)*MAX_QUERY_FMT_TIMES);

        for (i = 0; i < MAX_QUERY_FMT_TIMES; i++){
            if (mJpegEncoder->EnumJpegEncParam(SUPPORTED_FMT,&(mJpegEncoderSupportFmt[i])) < 0)
                break;
        }
        if (i == 0){
            CAMERA_LOG_ERR("Get the parameters error");
            return UNKNOWN_ERROR;
        }
        return ret;
    }
    int CameraHal :: NegotiateCaptureFmt(bool TakePicFlag)
    {
        CAMERA_LOG_FUNC;
        int ret = NO_ERROR, i = 0, j = 0;
        unsigned int nPickFormat = 0;

        if(TakePicFlag){
            //when take picture, only the mJpegEncoderSupportFmt impact it.
            for (i = 0; i < MAX_QUERY_FMT_TIMES; i++){
                for (j = 0; j < MAX_QUERY_FMT_TIMES; j++){
                    if (mJpegEncoderSupportFmt[j] == 0)
                        break;
                    if (mSensorSupportFmt[i] == mJpegEncoderSupportFmt[j]){
                        nPickFormat = mSensorSupportFmt[i];
                        CAMERA_LOG_INFO("Get the Picture Encode Format :%c%c%c%c\n",
                                nPickFormat&0xFF, (nPickFormat>>8)&0xFF,
                                (nPickFormat>>16)&0xFF, (nPickFormat>>24)&0xFF);
                        break;
                    }
                }
                if ((nPickFormat != 0) || (mSensorSupportFmt[i] == 0))
                    break;
            }
            if (nPickFormat == 0) {
                CAMERA_LOG_ERR("NegotiateCaptureFmt failed");
                return UNKNOWN_ERROR;
            }
            else {
                mPictureEncodeFormat = nPickFormat;
                mCaptureDeviceCfg.fmt = nPickFormat;
            }
            //should make mPictureEncodeFormat equal to mPreviewCapturedFormat.
            //because allocate buffer should use it.
            mPreviewCapturedFormat = mPictureEncodeFormat;
        }//endif TakePicFlag
        else{
            //when preview or encoder, only mVpuSupportFmt impact it.
            for(i =0; i< MAX_QUERY_FMT_TIMES; i ++){
                for (j = 0; j < MAX_VPU_SUPPORT_FORMAT; j++) {
                    if(mVpuSupportFmt[j] == 0)
                        break;
                    if(mSensorSupportFmt[i] == mVpuSupportFmt[j]) {
                        nPickFormat = mSensorSupportFmt[i];
                        CAMERA_LOG_RUNTIME("get the priview format:%c%c%c%c\n",
                                nPickFormat&0xFF, (nPickFormat>>8)&0xFF,
                                (nPickFormat>>16)&0xFF, (nPickFormat>>24)&0xFF);
                        break;
                    }
                }//end for
                if ((nPickFormat != 0) || (mSensorSupportFmt[i] == 0))
                    break;
            }
            if (nPickFormat == 0) {
                CAMERA_LOG_ERR("NegotiateCaptureFmt2 failed");
                return UNKNOWN_ERROR;
            }
            else {
                mPreviewCapturedFormat = nPickFormat;
                mCaptureDeviceCfg.fmt = nPickFormat;
            }
        }//end else

        return ret;
    }

    int CameraHal :: PrepareJpegEncoder()
    {
        int ret = NO_ERROR;
        struct jpeg_enc_make_info_t make_info;
        struct jpeg_enc_makernote_info_t makernote_info;
        struct jpeg_enc_model_info_t model_info;
        struct jpeg_enc_datetime_info_t datetime_info;
        struct jpeg_enc_focallength_t focallength_info;
        struct jpeg_enc_gps_param gps_info;
        int rotate_angle = 0;
        JPEG_ENCODER_WHITEBALANCE whitebalance_info;
        JPEG_ENCODER_FLASH flash_info;
        const char * pWhiteBalanceStr, *pFlashStr;

        char temp_string[30], gps_datetime_string[11];
        char format[30] = "%Y:%m:%d %k:%M:%S";
        time_t clock;
        struct tm *tm, *temp_tm;
        char * cLatitude, *cLongtitude, *cAltitude,*cTimeStamp;
        double dAltitude;

        mJpegEncCfg.BufFmt = mPictureEncodeFormat;
        mParameters.getPictureSize((int *)&(mJpegEncCfg.PicWidth), (int *)&(mJpegEncCfg.PicHeight));
        mJpegEncCfg.ThumbWidth = (unsigned int)mParameters.getInt(CameraParameters::KEY_JPEG_THUMBNAIL_WIDTH);
        mJpegEncCfg.ThumbHeight =(unsigned int)mParameters.getInt(CameraParameters::KEY_JPEG_THUMBNAIL_HEIGHT);
        CAMERA_LOG_INFO("pic width %d, height %d, fmt %d", mJpegEncCfg.PicWidth, mJpegEncCfg.PicHeight, mJpegEncCfg.BufFmt);
        CAMERA_LOG_INFO("thumbnail width is %d, height is %d", mJpegEncCfg.ThumbWidth, mJpegEncCfg.ThumbHeight);
        //set focallength info
        focallength_info.numerator=10001;
        focallength_info.denominator=1000;  // hardcode here for the cts
        mJpegEncCfg.pFoclLength = &focallength_info;

        //set the make info
        make_info.make_bytes=strlen(EXIF_MAKENOTE);
        strcpy((char *)make_info.make, EXIF_MAKENOTE);
        mJpegEncCfg.pMakeInfo = &make_info;

        //set makernote info
        makernote_info.makernote_bytes=strlen(EXIF_MAKENOTE);
        strcpy((char *)makernote_info.makernote, EXIF_MAKENOTE);
        mJpegEncCfg.pMakeNote = &makernote_info;

        //set model info
        model_info.model_bytes=strlen(EXIF_MODEL);
        strcpy((char *)model_info.model,EXIF_MODEL);
        mJpegEncCfg.pModelInfo = &model_info;

        //set datetime
        time(&clock);
        tm = localtime(&clock);
        time_t GpsUtcTime;
        strftime(temp_string, sizeof(temp_string), format, tm);
        CAMERA_LOG_INFO("date time: %s", temp_string);
        memcpy((char *)datetime_info.datetime, temp_string, sizeof(datetime_info.datetime));
        mJpegEncCfg.pDatetimeInfo = &datetime_info;

        rotate_angle = mParameters.getInt(CameraParameters::KEY_ROTATION);
        if (rotate_angle == 0)
            mJpegEncCfg.RotationInfo = ORIENTATION_NORMAL; //the android and the jpeg has the same define
        else if (rotate_angle == 90)
            mJpegEncCfg.RotationInfo = ORIENTATION_ROTATE_90;
        else if (rotate_angle == 180)
            mJpegEncCfg.RotationInfo = ORIENTATION_ROTATE_180;
        else if (rotate_angle == 270)
            mJpegEncCfg.RotationInfo = ORIENTATION_ROTATE_270;
        else
            mJpegEncCfg.RotationInfo = ORIENTATION_NORMAL;
        CAMERA_LOG_INFO("rotate: %d", rotate_angle);

        pWhiteBalanceStr = mParameters.get(CameraParameters::KEY_WHITE_BALANCE);
        CAMERA_LOG_INFO("white balance: %s",pWhiteBalanceStr);
        if (strcmp(pWhiteBalanceStr, CameraParameters::WHITE_BALANCE_AUTO) == 0){
            whitebalance_info = WHITEBALANCE_AUTO;
        }else{
            whitebalance_info = WHITEBALANCE_MANUAL;
        }
        mJpegEncCfg.WhiteBalanceInfo = whitebalance_info;

        pFlashStr = mParameters.get(CameraParameters::KEY_FLASH_MODE);
        CAMERA_LOG_INFO("flash mode: %s", pFlashStr);
        if (strcmp(pFlashStr, CameraParameters::FLASH_MODE_OFF) == 0){
            flash_info = FLASH_NOT_FIRE;
        }else if (strcmp(pFlashStr, CameraParameters::FLASH_MODE_AUTO) == 0){
            flash_info = FLASH_FIRED_AUTO;
        }else if (strcmp(pFlashStr, CameraParameters::FLASH_MODE_ON) == 0){
            flash_info = FLASH_FIRED;
        }else if (strcmp(pFlashStr, CameraParameters::FLASH_MODE_RED_EYE) == 0){
            flash_info = FLASH_FIRED_RED_EYE_REDUCE;
        }
        else if (strcmp(pFlashStr, CameraParameters::FLASH_MODE_TORCH) == 0){
            flash_info = FLASH_FIRED_COMPULOSORY;
        }
        else{
            flash_info = FLASH_NOT_FIRE;
        }
        mJpegEncCfg.FlashInfo = flash_info;

        cLatitude   = (char *)mParameters.get(CameraParameters::KEY_GPS_LATITUDE);
        cLongtitude = (char *)mParameters.get(CameraParameters::KEY_GPS_LONGITUDE);
        cAltitude   = (char *)mParameters.get(CameraParameters::KEY_GPS_ALTITUDE);
        cTimeStamp  = (char *)mParameters.get(CameraParameters::KEY_GPS_TIMESTAMP);

        if (cLatitude !=NULL && cLongtitude!=NULL && cAltitude!=NULL && cTimeStamp!=NULL){

            gps_info.version=0x02020000;

            //latitude: dd/1,mm/1,ss/1
            gps_info.latitude_degree[1]=1;
            gps_info.latitude_minute[1]=1;
            gps_info.latitude_second[1]=1000;
            memcpy((char *)gps_info.latitude_ref, (char *)"N ", sizeof(gps_info.latitude_ref));

            if (stringTodegree(cLatitude, gps_info.latitude_degree[0],gps_info.latitude_minute[0],gps_info.latitude_second[0])>0){
                //the ref is south
                memcpy((char *)gps_info.latitude_ref, (char *)"S ", sizeof(gps_info.latitude_ref));
            }

            //longtitude: dd/1,mm/1,ss/1
            gps_info.longtitude_degree[1]=1;
            gps_info.longtitude_minute[1]=1;
            gps_info.longtitude_second[1]=1000;
            memcpy((char *)gps_info.longtitude_ref, (char *)"E ", sizeof(gps_info.longtitude_ref));

            if (stringTodegree(cLongtitude, gps_info.longtitude_degree[0],gps_info.longtitude_minute[0],gps_info.longtitude_second[0])>0){
                //the ref is Weston
                memcpy((char *)gps_info.longtitude_ref, (char *)"W ", sizeof(gps_info.longtitude_ref));
            }

            //altitude(meters): aa/1
            gps_info.altitude_ref=0;		// 0: up sea level; 1: below sea level
            gps_info.altitude[0]=1000;
            gps_info.altitude[1]=1;
            if (cAltitude != NULL){
                int intValue;
                gps_info.altitude[1]=1000;	   // the precision is CM
                dAltitude= atof(cAltitude);
                CAMERA_LOG_RUNTIME("altitude: %s", cAltitude);
                intValue = (int)(dAltitude * 1000.0);
                if (intValue<0) {gps_info.altitude_ref = 1; intValue *= -1;}
                gps_info.altitude[0] = (unsigned long) intValue;
                CAMERA_LOG_RUNTIME("gps_info.altitude[0] is %u, gps_info.altitude_ref is %d", gps_info.altitude[0], gps_info.altitude_ref);
            }

            //timestamp: hh/1,mm/1,ss/1
            gps_info.hour[1]=1;
            gps_info.minute[1]=1;
            gps_info.seconds[1]=1;
            if (cTimeStamp != NULL){

                GpsUtcTime = atol(cTimeStamp);
                CAMERA_LOG_INFO("Timestamp: %s", cTimeStamp);
                temp_tm = gmtime((const time_t*)&GpsUtcTime);
                if (temp_tm != NULL)
                    tm = temp_tm;
            }

            gps_info.hour[0] = tm->tm_hour;
            gps_info.minute[0] = tm->tm_min;
            gps_info.seconds[0] = tm->tm_sec;

            strcpy (format, "%Y:%m:%d ");


            strftime((char *)temp_string, strlen(temp_string), format, tm);
            memcpy(gps_info.datestamp, temp_string, sizeof(gps_info.datestamp));


            char * processMehod = (char *)mParameters.get(CameraParameters::KEY_GPS_PROCESSING_METHOD);
            if (processMehod == NULL){
                CAMERA_LOG_INFO("processMethod is NULL, add a fake");
                processMehod = (char *)"fsl_fake_method";
            }

            memcpy(gps_info.processmethod, processMehod, strlen(processMehod));

            gps_info.processmethod_bytes=strlen(processMehod);

            CAMERA_LOG_INFO("processmethod: %s", gps_info.processmethod);

            mJpegEncCfg.pGps_info = &gps_info;
        }else{
            mJpegEncCfg.pGps_info = NULL;
        }

        if (mJpegEncoder->JpegEncoderInit(&mJpegEncCfg)< 0){
            CAMERA_LOG_ERR("Jpeg Encoder Init error !!!");
            return UNKNOWN_ERROR;
        }

        return ret;
    }

    status_t CameraHal::convertPreviewFormatToString(char *pStr, int length, unsigned int format)
    {
        CAMERA_LOG_FUNC;
        if(pStr == NULL || length < 10) {
            CAMERA_LOG_ERR("%s: invalide parameters", __FUNCTION__);
            return BAD_VALUE;
        }
        if(format == v4l2_fourcc('Y','U','1','2')) {
            strcpy(pStr, "yuv420p");
        }
        else if(format == v4l2_fourcc('N','V','1','2')) {
            strcpy(pStr, "yuv420sp");
        }
        //else if(format == v4l2_fourcc('Y','U','Y','V')) {
        //    strcpy(pStr, "yuv422i-yuyv");
        //}
        else {
            CAMERA_LOG_ERR("%s: Only YU12 or NV12 is supported", __FUNCTION__);
            return BAD_VALUE;
        }
        return NO_ERROR;
    }

    status_t CameraHal::convertStringToPreviewFormat(unsigned int *pFormat)
    {
        CAMERA_LOG_FUNC;
        if(!strcmp(mParameters.getPreviewFormat(), "yuv420p")) {
            *pFormat = v4l2_fourcc('Y','U','1','2');
        }
        else if(!strcmp(mParameters.getPreviewFormat(), "yuv420sp")) {
            *pFormat = v4l2_fourcc('N','V','1','2');
        }
        //else if(!strcmp(mParameters.getPreviewFormat(), "yuv422i-yuyv")) {
        //    *pFormat = v4l2_fourcc('Y','U','Y','V');
        //}
        else {
            CAMERA_LOG_ERR("Only yuv420sp or yuv420p is supported");
            return BAD_VALUE;
        }
        return NO_ERROR;
    }

    status_t CameraHal::CameraHALStartPreview()
    {
        CAMERA_LOG_FUNC;
        status_t ret = NO_ERROR;
        int  max_fps, min_fps;
        int actual_fps = 15;

        mParameters.getPreviewSize((int *)&(mCaptureDeviceCfg.width),(int *)&(mCaptureDeviceCfg.height));

        if ((ret = convertStringToPreviewFormat(&mPreviewCapturedFormat)) != 0) {
            CAMERA_LOG_ERR("%s: convertStringToPreviewFormat error", __FUNCTION__);
            return ret;
        }

        mCaptureDeviceCfg.fmt = mPreviewCapturedFormat;

        CAMERA_LOG_RUNTIME("*********%s,mCaptureDeviceCfg.fmt=%x************", __FUNCTION__, mCaptureDeviceCfg.fmt);
        mCaptureDeviceCfg.rotate = (SENSOR_PREVIEW_ROTATE)mPreviewRotate;
        //Default setting is 15FPS
        mCaptureDeviceCfg.tv.numerator = 1;
        mCaptureDeviceCfg.tv.denominator = 15;
        mCaptureDevice->GetDevName(mCameraSensorName);
        if (strstr(mCameraSensorName, "uvc") == NULL){
            //according to google's doc getPreviewFrameRate & getPreviewFpsRange should support both.
            // so here just a walkaround, if the app set the FpsRange, will follow this FpsRange.
            mParameters.getPreviewFpsRange(&min_fps, &max_fps);
            if (max_fps < 1000 || min_fps < 1000 || max_fps > 33000 || min_fps > 33000){
                if (mParameters.getPreviewFrameRate() >= 15){
                    mCaptureDeviceCfg.tv.denominator = mParameters.getPreviewFrameRate();
                    CAMERA_LOG_INFO("Set Preview Fps %d", mParameters.getPreviewFrameRate());
                }
            }
            else{
                CAMERA_LOG_INFO("Set Preview Fps Range %d - %d",min_fps, max_fps);
                actual_fps = min_fps > 15000? 30:15;
                mCaptureDeviceCfg.tv.denominator = actual_fps;
            }
        }else{
                mCaptureDeviceCfg.tv.denominator = 15;
        }
        mCaptureBufNum = PREVIEW_CAPTURE_BUFFER_NUM;
        mTakePicFlag = false;

        if(mCaptureDeviceCfg.fmt)
                mPreviewFrameSize = mCaptureDeviceCfg.width*mCaptureDeviceCfg.height*3/2;
            else
                mPreviewFrameSize = mCaptureDeviceCfg.width*mCaptureDeviceCfg.height *2;

        if ((ret = PrepareCaptureDevices()) < 0){
            CAMERA_LOG_ERR("PrepareCaptureDevices error ");
            return ret;
        }
        if ((ret = PreparePreviwBuf()) < 0){
            CAMERA_LOG_ERR("PreparePreviwBuf error");
            return ret;
        }

        if ((ret = PreparePreviwMisc()) < 0){
            CAMERA_LOG_ERR("PreparePreviwMisc error");
            return ret;
        }

        if(mNativeWindow != NULL) {
            if ((ret = CameraHALPreviewStart()) < 0){
                CAMERA_LOG_ERR("CameraHALPreviewStart error");
                return ret;
            }
        }

        return ret;
    }
    void CameraHal::CameraHALStopPreview()
    {
        CAMERA_LOG_FUNC;
        if (mPreviewRunning != 0)	{
            CameraHALStopThreads();
            CameraHALStopMisc();
            mCaptureBufNum = 0;
            CAMERA_LOG_INFO("camera hal stop preview done");
        }else{
            CAMERA_LOG_INFO("Camera hal already stop preview");
        }
        return ;
    }

    void CameraHal :: CameraHALStopThreads()
    {
        CAMERA_LOG_FUNC;

        mCaptureLock.lock();
        if(mCaptureRunning) {
            CAMERA_LOG_INFO("%s :capture run", __FUNCTION__);
            mCaptureThreadQueue.postStopMessage();
            mCaptureRunning = false;
            if(gettid()!= mCaptureFrameThread->mTID)
                sem_wait(&mCaptureStoppedCondition);
            else
                CAMERA_LOG_INFO("Stop CaptureFrameThread in itself");
        }else {
            CAMERA_LOG_INFO("%s :capture not run", __FUNCTION__);
        }
        mCaptureLock.unlock();
        CAMERA_LOG_INFO("%s :---------", __FUNCTION__);

        mPreviewLock.lock();
        if(mPreviewRunning) {
            CAMERA_LOG_INFO("%s :preview run", __FUNCTION__);
            mPreviewThreadQueue.postStopMessage();
            mPreviewRunning = false;
            if(gettid()!= mPreviewShowFrameThread->mTID)
                sem_wait(&mPreviewStoppedCondition);
            else
                CAMERA_LOG_INFO("Stop PreviewShowThread in itself");
        }else {
            CAMERA_LOG_INFO("%s :preview not run", __FUNCTION__);
        }
        mPreviewLock.unlock();
        CAMERA_LOG_INFO("%s :exit", __FUNCTION__);

        return ;
    }

    void CameraHal :: CameraHALStopMisc()
    {
        CAMERA_LOG_FUNC;

        mCaptureDevice->DevStop();
        mCaptureDevice->DevDeAllocate();
        freeBuffersToNativeWindow();
        if(mSensorType == CAMERA_TYPE_UVC) {
            CloseCaptureDevice();
        }
    }

    status_t CameraHal :: PrepareCaptureBufs()
    {
        CAMERA_LOG_FUNC;
        unsigned int CaptureBufNum = mCaptureBufNum;

        if(allocateBuffersFromNativeWindow() < 0) {
            CAMERA_LOG_ERR("allocateBuffersFromNativeWindow error");
            return BAD_VALUE;
        }

        if (mCaptureDevice->DevRegisterBufs(mCaptureBuffers,&CaptureBufNum)< 0){
            CAMERA_LOG_ERR("capture device allocat buf error");
            return BAD_VALUE;
        }
        if(mCaptureBufNum != CaptureBufNum){
            CAMERA_LOG_INFO("The driver can only supply %d bufs, but required %d bufs", CaptureBufNum, mCaptureBufNum);
        }

        mCaptureBufNum = CaptureBufNum;

        if (mCaptureDevice->DevPrepare()< 0){
            CAMERA_LOG_ERR("capture device prepare error");
            return BAD_VALUE;
        }
        nCameraBuffersQueued = mCaptureBufNum;
        mIsCaptureBufsAllocated = 1;

        if((AllocateRecordVideoBuf())<0) {
            CAMERA_LOG_INFO("%s: AllocateRecordVideoBuf error\n", __FUNCTION__);
            return BAD_VALUE;
        }

        return NO_ERROR;
    }

    status_t CameraHal :: PrepareCaptureDevices()
    {
        CAMERA_LOG_FUNC;
        status_t ret = NO_ERROR;
        int i =0;
        struct capture_config_t *pCapcfg;
        if ((ret = OpenCaptureDevice())<0)
            return ret;

        if (mCaptureDevice->DevSetConfig(&mCaptureDeviceCfg) < 0) {//set the config and get the captured framesize
            CAMERA_LOG_ERR("Dev config failed");
            return BAD_VALUE;
        }
        mCaptureFrameSize = mCaptureDeviceCfg.framesize;

        if(mNativeWindow != 0) {
            if(PrepareCaptureBufs() < 0) {
                CAMERA_LOG_ERR("PrepareCaptureBufs() error");
                return BAD_VALUE;
            }
        }

        return ret;
    }

    status_t CameraHal::PreparePreviwBuf()
    {
        CAMERA_LOG_FUNC;
        status_t ret = NO_ERROR;
        unsigned int i =0;

        //temply hard code here
        if (mTakePicFlag == 0){
            if(mPreviewMemory != NULL) {
                mPreviewMemory->release(mPreviewMemory);
            }

            mPreviewMemory = mRequestMemory(-1, mPreviewFrameSize, mPreviewHeapBufNum, NULL);
            if(mPreviewMemory == NULL) {
                CAMERA_LOG_ERR("%s, allocate memory failed", __FUNCTION__);
                return NO_MEMORY;
            }
        }
        return ret;
    }

    status_t CameraHal ::PreparePreviwMisc()
    {
        CAMERA_LOG_FUNC;
        status_t ret = NO_ERROR;
        preview_heap_buf_head = 0;

       return ret;
    }

    status_t CameraHal ::CameraHALPreviewStart()
    {
        CAMERA_LOG_FUNC;
        status_t ret = NO_ERROR;
        if (mCaptureDevice->DevStart()<0)
            return INVALID_OPERATION;

        mIsCaptureBufsAllocated = 1;

        unsigned int bufIndex = 0;
        //skip 10 frames when doing preview
        if(mSensorType != CAMERA_TYPE_UVC) {
            for (int k = 0; k < 10; k++) {
                mCaptureDevice->DevDequeue(&bufIndex);
                mCaptureDevice->DevQueue(bufIndex);
            }
        }

        for(unsigned int i=0; i < mCaptureBufNum; i++) {
            mCaptureThreadQueue.postMessage(new CMessage(CMESSAGE_TYPE_NORMAL, i));
        }
        return ret;
    }

    status_t CameraHal::putBufferCount(DMA_BUFFER *pBuf)
    {
        if(pBuf == NULL)
            return INVALID_OPERATION;

        unsigned int buf_index = pBuf - &mCaptureBuffers[0];

        Mutex::Autolock _l(pBuf->mBufferLock);
        if(!mRecordRunning &&  (mVideoBufferUsing[buf_index] == 1)
                && (pBuf->refCount == 2)) {
            pBuf->refCount --;
            mVideoBufferUsing[buf_index] = 0;
        }

        pBuf->refCount --;
        if(pBuf->refCount == 0) {
            if(mCaptureRunning) {
                if(buf_index < mCaptureBufNum) {
                    if(mCaptureDevice->DevQueue(buf_index) <0){
                        CAMERA_LOG_ERR("The Capture device queue buf error !!!!");
                        return INVALID_OPERATION;
                    }
                    //CAMERA_LOG_RUNTIME("Return buffer %d to Capture Device", buf_index);
                    mCaptureBuffers[buf_index].refCount = 0;
                    nCameraBuffersQueued++;
                    mEnqueuedBufs --;
                    mCaptureThreadQueue.postMessage(new CMessage(CMESSAGE_TYPE_NORMAL, buf_index));
                }else {
                    return INVALID_OPERATION;
                }
            }//end elseif
        }
        return NO_ERROR;
    }

    void CameraHal ::getBufferCount(DMA_BUFFER *pBuf)
    {
        if(pBuf == NULL)
            return;
        Mutex::Autolock _l(pBuf->mBufferLock);
        pBuf->refCount ++;
    }

    int CameraHal ::captureframeThreadWrapper()
    {
        CAMERA_LOG_FUNC;
        status_t ret = NO_ERROR;
        while(1) {
            if(mExitCaptureThread) {
                CAMERA_LOG_INFO("%s: exiting normally", __FUNCTION__);
                return 0;
            }
            ret = captureframeThread();
            if(ret < 0) {
                CAMERA_LOG_ERR("%s: capture thread exit with exception", __FUNCTION__);
                return ret;
            }
        }
        return ret;
    }

    int CameraHal ::captureframeThread()
    {
        //CAMERA_LOG_FUNC;

        unsigned int bufIndex = -1;
        status_t ret = NO_ERROR;
        sp<CMessage> msg = mCaptureThreadQueue.waitMessage();
        if(msg == 0) {
            CAMERA_LOG_ERR("%s: get invalide message", __FUNCTION__);
            return BAD_VALUE;
        }

        switch(msg->what) {
            case CMESSAGE_TYPE_NORMAL:
                ret = mCaptureDevice->DevDequeue(&bufIndex);
                //handle the error return.
                if(ret < 0) {
                    CAMERA_LOG_ERR("%s: get invalide buffer", __FUNCTION__);
                    //mCaptureRunning = false;
                    mCaptureThreadQueue.clearMessage();
                    sem_post(&mCaptureStoppedCondition);
                    return NO_ERROR;
                }
                //CAMERA_LOG_RUNTIME("Get buffer %d from Capture Device", bufIndex);
                //handle the normal return.
                getBufferCount(&mCaptureBuffers[bufIndex]);
                mPreviewThreadQueue.postMessage(new CMessage(CMESSAGE_TYPE_NORMAL, bufIndex));

                if(mRecordRunning) {
                    mEncodeThreadQueue.postMessage(new CMessage(CMESSAGE_TYPE_NORMAL, bufIndex));
                }
                break;
            case CMESSAGE_TYPE_STOP:
                CAMERA_LOG_INFO("%s: capture thread stop", __FUNCTION__);
                mCaptureThreadQueue.clearMessage();
                sem_post(&mCaptureStoppedCondition);
                CAMERA_LOG_INFO("%s: capture thread stop finish", __FUNCTION__);
                break;
            case CMESSAGE_TYPE_QUITE:
                mExitCaptureThread = 1;
                CAMERA_LOG_INFO("%s: receive QUITE message", __FUNCTION__);
                if (mExitPreviewThread == 0)
                    mPreviewThreadQueue.postQuitMessage();
                if (mExitEncodeThread == 0)
                    mEncodeThreadQueue.postQuitMessage();
                break;
            default:
                CAMERA_LOG_ERR("%s: wrong msg type %d", __FUNCTION__, msg->what);
                ret = INVALID_OPERATION;
                break;
        }//end switch

        return ret;
    }

    void CameraHal::SearchBuffer(void *pNativeBuf, int *pIndex)
    {
        //int index;
        for(unsigned int i=0; i < mCaptureBufNum; i++){
            if(mCaptureBuffers[i].native_buf == pNativeBuf) {
                *pIndex = i;
                return;
            }
        }

        *pIndex = -1;
        return;
    }

    int CameraHal ::previewshowFrameThreadWrapper()
    {
        CAMERA_LOG_FUNC;
        status_t ret = NO_ERROR;
        while(1) {
            if(mExitPreviewThread) {
                CAMERA_LOG_INFO("%s: exiting", __FUNCTION__);
                return 0;
            }
            ret = previewshowFrameThread();
            if(ret < 0) {
                CAMERA_LOG_ERR("%s: preview thread exit with exception", __FUNCTION__);
                mExitPreviewThread = 1;
                return ret;
            }
        }
        return ret;
    }

    static void bufferDump(DMA_BUFFER *pBufs)
    {
#ifdef FSL_CAMERAHAL_DUMP
            //for test code
            char value[10] = {0};
            static int vflg = 0;
            property_get("rw.camera.test", value, "");
            if(strcmp(value, "1") == 0)
                vflg = 1;
            if(vflg){
                FILE *pf = NULL;
                pf = fopen("/sdcard/camera_tst.data", "wb");
                if(pf == NULL) {
                    CAMERA_LOG_ERR("open /sdcard/camera_tst.data failed");
                }
                else {
                    fwrite(pInBuf->virt_start, pInBuf->length, 1, pf);
                    fclose(pf);
                }
                vflg = 0;
            }
#endif
    }

    int CameraHal ::previewshowFrameThread()
    {
        //CAMERA_LOG_FUNC;
        status_t ret = NO_ERROR;
        int display_index = -1;
        DMA_BUFFER *pInBuf = NULL;
        buffer_handle_t *buf_h = NULL;
        int buf_index = -1;
        int stride = 0, err = 0;

        sp<CMessage> msg = mPreviewThreadQueue.waitMessage();
        if(msg == 0) {
            CAMERA_LOG_ERR("%s: get invalide message", __FUNCTION__);
            mPreviewRunning = false;
            return BAD_VALUE;
        }

        switch(msg->what) {
            case CMESSAGE_TYPE_NORMAL:
                display_index = msg->arg0;
                if(display_index < 0 || (unsigned int)display_index >= mCaptureBufNum) {
                    CAMERA_LOG_ERR("%s: get invalide buffer index", __FUNCTION__);
                    mPreviewRunning = false;
                    mPreviewThreadQueue.clearMessage();
                    sem_post(&mPreviewStoppedCondition);
                    return BAD_VALUE;
                }
                pInBuf = &mCaptureBuffers[display_index];

                if (mMsgEnabled & CAMERA_MSG_PREVIEW_FRAME) {
                    //CAMERA_LOG_ERR("*******CAMERA_MSG_PREVIEW_FRAME*******");
                    convertNV12toYUV420SP((uint8_t*)(pInBuf->virt_start),
                            (uint8_t*)((unsigned char*)mPreviewMemory->data + preview_heap_buf_head*mPreviewFrameSize),mCaptureDeviceCfg.width, mCaptureDeviceCfg.height);
                    mDataCb(CAMERA_MSG_PREVIEW_FRAME, mPreviewMemory, preview_heap_buf_head, NULL, mCallbackCookie);
                    preview_heap_buf_head ++;
                    preview_heap_buf_head %= mPreviewHeapBufNum;
                }

                if (mNativeWindow != 0) {
                    if (mNativeWindow->enqueue_buffer(mNativeWindow, (buffer_handle_t * )pInBuf->native_buf) < 0){
                        CAMERA_LOG_ERR("queueBuffer failed. May be bcos stream was not turned on yet.");
                        mPreviewRunning = false;
                        mPreviewThreadQueue.clearMessage();
                        sem_post(&mPreviewStoppedCondition);
                        return BAD_VALUE;
                    }
                    pInBuf->buf_state = WINDOW_BUFS_QUEUED;
                    mEnqueuedBufs ++;
                    bufferDump(pInBuf);
                    if (mEnqueuedBufs <= 2) {
                        return NO_ERROR;
                    }
                }
                else {
                    mPreviewRunning = false;
                    mPreviewThreadQueue.clearMessage();
                    sem_post(&mPreviewStoppedCondition);
                    return BAD_VALUE;
                }

                err = mNativeWindow->dequeue_buffer(mNativeWindow, &buf_h, &stride);
                if((err != 0) || buf_h == NULL) {
                    CAMERA_LOG_ERR("%s: dequeueBuffer failed.", __FUNCTION__);
                    mPreviewRunning = false;
                    mPreviewThreadQueue.clearMessage();
                    sem_post(&mPreviewStoppedCondition);
                    return INVALID_OPERATION;
                }

                SearchBuffer((void *)buf_h, &buf_index);

                if(buf_index >= (int)mCaptureBufNum || (buf_index < 0)) {
                    mNativeWindow->cancel_buffer(mNativeWindow, buf_h);
                    CAMERA_LOG_ERR("dequeue invalide buffer!!!!");
                    mPreviewRunning = false;
                    mPreviewThreadQueue.clearMessage();
                    sem_post(&mPreviewStoppedCondition);
                    return INVALID_OPERATION;
                }

                mCaptureBuffers[buf_index].buf_state = WINDOW_BUFS_DEQUEUED;
                ret = putBufferCount(&mCaptureBuffers[buf_index]);
                break;
            case CMESSAGE_TYPE_STOP:
                CAMERA_LOG_INFO("%s: preview thread stop", __FUNCTION__);
                mPreviewThreadQueue.clearMessage();
                sem_post(&mPreviewStoppedCondition);
                CAMERA_LOG_INFO("%s: preview thread stop finish", __FUNCTION__);
                break;
            case CMESSAGE_TYPE_QUITE:
                mExitPreviewThread = 1;
                CAMERA_LOG_INFO("%s: receive QUIT message", __FUNCTION__);
                break;
            default:
                CAMERA_LOG_ERR("%s: wrong msg type %d", __FUNCTION__, msg->what);
                ret = INVALID_OPERATION;
                break;
        }

        return ret;
    }

    int CameraHal::encodeframeThreadWrapper()
    {
        //CAMERA_LOG_FUNC;
        status_t ret = NO_ERROR;
        while(1) {
            if(mExitEncodeThread) {
                return 0;
            }
            ret = encodeframeThread();
            if(ret < 0) {
                CAMERA_LOG_ERR("%s: encode thread exit with exception", __FUNCTION__);
                mExitEncodeThread = 1;
                return ret;
            }
        }
        return ret;
    }

    int CameraHal::encodeframeThread()
    {
        //CAMERA_LOG_FUNC;
        status_t ret = NO_ERROR;
        int enc_index;
        sp<CMessage> msg = mEncodeThreadQueue.waitMessage();
        if(msg == 0) {
            CAMERA_LOG_ERR("%s: get invalide message", __FUNCTION__);
            return BAD_VALUE;
        }

        switch(msg->what) {
            case CMESSAGE_TYPE_NORMAL:
                enc_index = msg->arg0;
                unsigned int i;
                if(enc_index < 0 || (unsigned int)enc_index >= mCaptureBufNum) {
                    CAMERA_LOG_ERR("%s: get invalide buffer index", __FUNCTION__);
                    mRecordRunning = false;
                    mEncodeThreadQueue.clearMessage();
                    //sem_post(&mEncodeStoppedCondition);
                    return BAD_VALUE;
                }

                struct timespec ts;
                DMA_BUFFER *EncBuf;
                EncBuf = &mCaptureBuffers[enc_index];

                if ((mMsgEnabled & CAMERA_MSG_VIDEO_FRAME) && mRecordRunning) {
                    nsecs_t timeStamp = systemTime(SYSTEM_TIME_MONOTONIC);
                    if (mDirectInput == true) {
	                    memcpy((unsigned char*)mVideoMemory->data + enc_index*mPreviewFrameSize,
                            (void*)&mVideoBufferPhy[enc_index], sizeof(VIDEOFRAME_BUFFER_PHY));
                    } else {
                        memcpy((unsigned char*)mVideoMemory->data + enc_index*mPreviewFrameSize,
                                (void*)EncBuf->virt_start, mPreviewFrameSize);
                        ret = putBufferCount(EncBuf);
                    }

                    getBufferCount(&mCaptureBuffers[enc_index]);
                    mVideoBufferUsing[enc_index] = 1;
                    mDataCbTimestamp(timeStamp, CAMERA_MSG_VIDEO_FRAME, mVideoMemory, enc_index, mCallbackCookie);
                    break;
                }
                break;

            case CMESSAGE_TYPE_STOP:
                CAMERA_LOG_INFO("%s: encode thread stop", __FUNCTION__);
                mEncodeThreadQueue.clearMessage();
                //sem_post(&mEncodeStoppedCondition);
                break;
            case CMESSAGE_TYPE_QUITE:
                mExitEncodeThread = 1;
                break;

            default:
                CAMERA_LOG_ERR("%s: wrong msg type %d", __FUNCTION__, msg->what);
                ret = INVALID_OPERATION;
                break;
        }

        return ret;
    }

    status_t CameraHal :: AllocateRecordVideoBuf()
    {
        status_t ret = NO_ERROR;
        unsigned int i = 0;
        if(mVideoMemory != NULL) {
            mVideoMemory->release(mVideoMemory);
        }

        CAMERA_LOG_RUNTIME("Init the video Memory size %d", mPreviewFrameSize);
        mVideoMemory = mRequestMemory(-1, mPreviewFrameSize, mVideoBufNume, NULL);
        if(mVideoMemory == NULL) {
            CAMERA_LOG_ERR("%s, request video buffer failed", __FUNCTION__);
            return NO_MEMORY;
        }

        //Make sure the buffer been updated for direct input
        updateDirectInput(mDirectInput);
        return ret;
    }


    void CameraHal :: LockWakeLock()
    {
        if (!mPowerLock) {
            acquire_wake_lock (PARTIAL_WAKE_LOCK, V4LSTREAM_WAKE_LOCK);
            mPowerLock = true;
        }
    }
    void CameraHal :: UnLockWakeLock()
    {
        if (mPowerLock) {
            release_wake_lock (V4LSTREAM_WAKE_LOCK);
            mPowerLock = false;
        }
    }

    void CameraHal::convertNV12toYUV420SP(uint8_t *inputBuffer, uint8_t *outputBuffer, int width, int height)
    {
        /* Color space conversion from I420 to YUV420SP */
        int Ysize = 0, UVsize = 0;
        uint8_t *Yin, *Uin, *Vin, *Yout, *Uout, *Vout;

        Ysize = width * height;
        UVsize = width *  height >> 2;

        Yin = inputBuffer;
        Uin = Yin + Ysize;
        Vin = Uin + 1;

        Yout = outputBuffer;
        Vout = Yout + Ysize;
        Uout = Vout + 1;

        memcpy(Yout, Yin, Ysize);

        for(int k = 0; k < UVsize; k++) {
            *Uout = *Uin;
            *Vout = *Vin;
            Uout += 2;
            Vout += 2;
            Uin  += 2;
            Vin += 2;
        }
    }



    int CameraHal::stringTodegree(char* cAttribute, unsigned int &degree, unsigned int &minute, unsigned int &second)
    {
        double dAttribtute;
        double eAttr;
        long intAttribute;
        int ret  = 0;
        if (cAttribute == NULL){
            return -1;
        }

        CAMERA_LOG_RUNTIME("the attribute is %s", cAttribute);

        dAttribtute = atof(cAttribute);

        CAMERA_LOG_RUNTIME("the double of the attribute is %lf", dAttribtute);
        intAttribute  = (long)(dAttribtute*(double)3600.0);
        if (intAttribute < 0){
            ret = 1;
            intAttribute *=-1;
            dAttribtute *=-1;
            eAttr = dAttribtute - (double)((double)intAttribute/(double)3600.0);
            eAttr = eAttr * (double)3600.0 *(double)1000.0;
        }else {
            eAttr = dAttribtute - (double)((double)intAttribute/(double)3600.0);
            eAttr = eAttr * (double)3600.0 *(double)1000.0;
        }

        second = (unsigned int)(intAttribute%60);
        minute = (unsigned int)((intAttribute%3600-second)/60);
        degree = (unsigned int)(intAttribute/3600);
        second = (unsigned int)eAttr + second * 1000;

        CAMERA_LOG_RUNTIME("the degree is %u, %u, %u", degree,minute,second);

        return ret;

    }

};


