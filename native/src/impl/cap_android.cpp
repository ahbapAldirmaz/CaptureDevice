#include "precomp.hpp"

#ifdef HAVE_ANDROID_NATIVE_CAMERA

#include <opencv2/imgproc/imgproc.hpp>
#include <pthread.h>
#include <android/log.h>
#include <camera_activity.hpp>

#if !defined(LOGD) && !defined(LOGI) && !defined(LOGE)
#define LOG_TAG "CV_CAP"
#define LOGD(...) ((void)__android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__))
#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__))
#define LOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__))
#endif

class HighguiAndroidCameraActivity;

class CvCapture_Android : public CvCapture
{
public:
    CvCapture_Android(int);
    virtual ~CvCapture_Android();

    virtual double getProperty(int propIdx);
    virtual bool setProperty(int probIdx, double propVal);
    virtual bool grabFrame();
    virtual IplImage* retrieveFrame(int outputType);
    virtual int getCaptureDomain() { return CV_CAP_ANDROID; }

    bool isOpened() const;

protected:
    struct OutputMap
    {
    public:
        cv::Mat mat;
        IplImage* getIplImagePtr();
    private:
        IplImage iplHeader;
    };

    CameraActivity* m_activity;

    //raw from camera
    int m_width;
    int m_height;
    unsigned char *m_frameYUV420;
    unsigned char *m_frameYUV420next;

    enum YUVformat
    {
        noformat = 0,
        yuv420sp,
        yuv420i,
        yuvUnknown
    };

    YUVformat m_frameFormat;

    void setFrame(const void* buffer, int bufferSize);

private:
    bool m_isOpened;
    bool m_CameraParamsChanged;

    //frames counter for statistics
    int m_framesGrabbed;

    //cached converted frames
    OutputMap m_frameGray;
    OutputMap m_frameColor;
    bool m_hasGray;
    bool m_hasColor;

    enum CvCapture_Android_DataState {
	    CVCAPTURE_ANDROID_STATE_NO_FRAME=0,
	    CVCAPTURE_ANDROID_STATE_HAS_NEW_FRAME_UNGRABBED,
	    CVCAPTURE_ANDROID_STATE_HAS_FRAME_GRABBED
    };
    volatile CvCapture_Android_DataState m_dataState;

    //synchronization
    pthread_mutex_t m_nextFrameMutex;
    pthread_cond_t m_nextFrameCond;
    volatile bool m_waitingNextFrame;
    volatile bool m_shouldAutoGrab;

    void prepareCacheForYUV(int width, int height);
    bool convertYUV2Grey(int width, int height, const unsigned char* yuv, cv::Mat& resmat);
    bool convertYUV2BGR(int width, int height, const unsigned char* yuv, cv::Mat& resmat, bool inRGBorder, bool withAlpha);

    friend class HighguiAndroidCameraActivity;
};


class HighguiAndroidCameraActivity : public CameraActivity
{
public:
    HighguiAndroidCameraActivity(CvCapture_Android* capture)
    {
        m_capture = capture;
        m_framesReceived = 0;
    }

    virtual bool onFrameBuffer(void* buffer, int bufferSize)
    {
        if(isConnected() && buffer != 0 && bufferSize > 0)
        {
            m_framesReceived++;
            if (m_capture->m_waitingNextFrame || m_capture->m_shouldAutoGrab)
            {
                pthread_mutex_lock(&m_capture->m_nextFrameMutex);

                m_capture->setFrame(buffer, bufferSize);

                pthread_cond_broadcast(&m_capture->m_nextFrameCond);
                pthread_mutex_unlock(&m_capture->m_nextFrameMutex);
            }
            return true;
        }
        return false;
    }

    void LogFramesRate()
    {
        LOGI("FRAMES received: %d  grabbed: %d", m_framesReceived, m_capture->m_framesGrabbed);
    }

private:
    CvCapture_Android* m_capture;
    int m_framesReceived;
};

IplImage* CvCapture_Android::OutputMap::getIplImagePtr()
{
    if( mat.empty() )
        return 0;

    iplHeader = IplImage(mat);
    return &iplHeader;
}

CvCapture_Android::CvCapture_Android(int cameraId)
{
    //defaults
    m_width               = 0;
    m_height              = 0;
    m_activity            = 0;
    m_isOpened            = false;
    m_frameYUV420        = 0;
    m_frameYUV420next    = 0;
    m_hasGray             = false;
    m_hasColor            = false;
    m_dataState           = CVCAPTURE_ANDROID_STATE_NO_FRAME;
    m_waitingNextFrame    = false;
    m_shouldAutoGrab      = false;
    m_framesGrabbed       = 0;
    m_CameraParamsChanged = false;
    m_frameFormat         = noformat;

    //try connect to camera
    m_activity = new HighguiAndroidCameraActivity(this);

    if (m_activity == 0) return;

    pthread_mutex_init(&m_nextFrameMutex, NULL);
    pthread_cond_init (&m_nextFrameCond,  NULL);

    CameraActivity::ErrorCode errcode = m_activity->connect(cameraId);

    if(errcode == CameraActivity::NO_ERROR)
        m_isOpened = true;
    else
    {
        LOGE("Native_camera returned opening error: %d", errcode);
        delete m_activity;
        m_activity = 0;
    }
}

bool CvCapture_Android::isOpened() const
{
    return m_isOpened;
}

CvCapture_Android::~CvCapture_Android()
{
    if (m_activity)
    {
        ((HighguiAndroidCameraActivity*)m_activity)->LogFramesRate();


	pthread_mutex_lock(&m_nextFrameMutex);

        unsigned char *tmp1=m_frameYUV420;
        unsigned char *tmp2=m_frameYUV420next;
        m_frameYUV420 = 0;
        m_frameYUV420next = 0;
        delete tmp1;
        delete tmp2;

	m_dataState=CVCAPTURE_ANDROID_STATE_NO_FRAME;
	pthread_cond_broadcast(&m_nextFrameCond);

	pthread_mutex_unlock(&m_nextFrameMutex);

        //m_activity->disconnect() will be automatically called inside destructor;
        delete m_activity;
        m_activity = 0;

        pthread_mutex_destroy(&m_nextFrameMutex);
        pthread_cond_destroy(&m_nextFrameCond);
    }
}

double CvCapture_Android::getProperty( int propIdx )
{
  switch ( propIdx )
    {
    case CV_CAP_PROP_FRAME_WIDTH:
        return (double)m_activity->getFrameWidth();
    case CV_CAP_PROP_FRAME_HEIGHT:
        return (double)m_activity->getFrameHeight();

    case CV_CAP_PROP_SUPPORTED_PREVIEW_SIZES_STRING:
	return (double)m_activity->getProperty(ANDROID_CAMERA_PROPERTY_SUPPORTED_PREVIEW_SIZES_STRING);
    case CV_CAP_PROP_PREVIEW_FORMAT:
        return (double)m_activity->getProperty(ANDROID_CAMERA_PROPERTY_PREVIEW_FORMAT_STRING);
    default:
        CV_Error( CV_StsOutOfRange, "Failed attempt to GET unsupported camera property." );
        break;
    }
    return -1.0;
}

bool CvCapture_Android::setProperty( int propIdx, double propValue )
{
    bool res = false;
    if( isOpened() )
    {
        switch ( propIdx )
        {
        case CV_CAP_PROP_FRAME_WIDTH:
            m_activity->setProperty(ANDROID_CAMERA_PROPERTY_FRAMEWIDTH, propValue);
            break;
        case CV_CAP_PROP_FRAME_HEIGHT:
            m_activity->setProperty(ANDROID_CAMERA_PROPERTY_FRAMEHEIGHT, propValue);
            break;

        case CV_CAP_PROP_AUTOGRAB:
	    m_shouldAutoGrab=(propValue != 0);
            break;

        default:
            CV_Error( CV_StsOutOfRange, "Failed attempt to SET unsupported camera property." );
	    return false;
        }

	if (propIdx != CV_CAP_PROP_AUTOGRAB) {// property for highgui class CvCapture_Android only
		m_CameraParamsChanged = true;
	}
	res = true;
    }

    return res;
}

bool CvCapture_Android::grabFrame()
{
    if( !isOpened() ) {
	    LOGE("CvCapture_Android::grabFrame(): camera is not opened");
	    return false;
    }

    bool res=false;
    pthread_mutex_lock(&m_nextFrameMutex);
    if (m_CameraParamsChanged)
    {
        m_activity->applyProperties();
        m_CameraParamsChanged = false;
	m_dataState= CVCAPTURE_ANDROID_STATE_NO_FRAME;//we will wait new frame
    }

    if (m_dataState!=CVCAPTURE_ANDROID_STATE_HAS_NEW_FRAME_UNGRABBED) {
	    m_waitingNextFrame = true;
	    pthread_cond_wait(&m_nextFrameCond, &m_nextFrameMutex);
    }

    if (m_dataState == CVCAPTURE_ANDROID_STATE_HAS_NEW_FRAME_UNGRABBED) {
            //LOGD("CvCapture_Android::grabFrame: get new frame");
	    //swap current and new frames
            unsigned char* tmp = m_frameYUV420;
            m_frameYUV420 = m_frameYUV420next;
            m_frameYUV420next = tmp;

	    //discard cached frames
	    m_hasGray = false;
	    m_hasColor = false;

	    m_dataState=CVCAPTURE_ANDROID_STATE_HAS_FRAME_GRABBED;
	    m_framesGrabbed++;

	    res=true;
    } else {
	    LOGE("CvCapture_Android::grabFrame: NO new frame");
    }


    int res_unlock=pthread_mutex_unlock(&m_nextFrameMutex);
    if (res_unlock) {
	    LOGE("Error in CvCapture_Android::grabFrame: pthread_mutex_unlock returned %d --- probably, this object has been destroyed", res_unlock);
	    return false;
    }

    return res;
}

IplImage* CvCapture_Android::retrieveFrame( int outputType )
{
    IplImage* image = NULL;

    unsigned char *current_frameYUV420=m_frameYUV420;
    //Attention! all the operations in this function below should occupy less time than the period between two frames from camera
    if (NULL != current_frameYUV420)
    {
        if (m_frameFormat == noformat)
        {
            union {double prop; const char* name;} u;
            u.prop = getProperty(CV_CAP_PROP_PREVIEW_FORMAT);
            if (0 == strcmp(u.name, "yuv420sp"))
                m_frameFormat = yuv420sp;
            else if (0 == strcmp(u.name, "yuv420i"))
                m_frameFormat = yuv420i;
            else
                m_frameFormat = yuvUnknown;
        }

        switch(outputType)
        {
        case CV_CAP_ANDROID_GREY_FRAME:
            if (!m_hasGray)
                if (!(m_hasGray = convertYUV2Grey(m_width, m_height, current_frameYUV420, m_frameGray.mat)))
                    return NULL;
            image = m_frameGray.getIplImagePtr();
            break;
        case CV_CAP_ANDROID_COLOR_FRAME_BGR: case CV_CAP_ANDROID_COLOR_FRAME_RGB:
            if (!m_hasColor)
                if (!(m_hasColor = convertYUV2BGR(m_width, m_height, current_frameYUV420, m_frameColor.mat, outputType == CV_CAP_ANDROID_COLOR_FRAME_RGB, false)))
                    return NULL;
            image = m_frameColor.getIplImagePtr();
            break;
        case CV_CAP_ANDROID_COLOR_FRAME_BGRA: case CV_CAP_ANDROID_COLOR_FRAME_RGBA:
            if (!m_hasColor)
                if (!(m_hasColor = convertYUV2BGR(m_width, m_height, current_frameYUV420, m_frameColor.mat, outputType == CV_CAP_ANDROID_COLOR_FRAME_RGBA, true)))
                    return NULL;
            image = m_frameColor.getIplImagePtr();
            break;
        default:
            LOGE("Unsupported frame output format: %d", outputType);
            CV_Error( CV_StsOutOfRange, "Output frame format is not supported." );
            image = NULL;
            break;
        }
    }
    return image;
}

//Attention: this method should be called inside pthread_mutex_lock(m_nextFrameMutex) only
void CvCapture_Android::setFrame(const void* buffer, int bufferSize)
{
    int width = m_activity->getFrameWidth();
    int height = m_activity->getFrameHeight();
    int expectedSize = (width * height * 3) >> 1;

    if ( expectedSize != bufferSize)
    {
        LOGE("ERROR reading YUV buffer: width=%d, height=%d, size=%d, receivedSize=%d", width, height, expectedSize, bufferSize);
        return;
    }

    //allocate memory if needed
    prepareCacheForYUV(width, height);

    //copy data
    memcpy(m_frameYUV420next, buffer, bufferSize);
    //LOGD("CvCapture_Android::setFrame -- memcpy is done");

#if 0 //moved this part of code into grabFrame
    //swap current and new frames
    unsigned char* tmp = m_frameYUV420;
    m_frameYUV420 = m_frameYUV420next;
    m_frameYUV420next = tmp;

    //discard cached frames
    m_hasGray = false;
    m_hasColor = false;
#endif

    m_dataState = CVCAPTURE_ANDROID_STATE_HAS_NEW_FRAME_UNGRABBED;
    m_waitingNextFrame = false;//set flag that no more frames required at this moment
}

//Attention: this method should be called inside pthread_mutex_lock(m_nextFrameMutex) only
void CvCapture_Android::prepareCacheForYUV(int width, int height)
{
    if (width != m_width || height != m_height)
    {
        LOGD("CvCapture_Android::prepareCacheForYUV: Changing size of buffers: from width=%d height=%d to width=%d height=%d", m_width, m_height, width, height);
        m_width = width;
        m_height = height;
        unsigned char *tmp = m_frameYUV420next;
        m_frameYUV420next = new unsigned char [width * height * 3 / 2];
	if (tmp != NULL) {
		delete[] tmp;
	}

        tmp = m_frameYUV420;
        m_frameYUV420 = new unsigned char [width * height * 3 / 2];
	if (tmp != NULL) {
		delete[] tmp;
	}
    }
}

bool CvCapture_Android::convertYUV2Grey(int width, int height, const unsigned char* yuv, cv::Mat& resmat)
{
    if (yuv == 0) return false;
    if (m_frameFormat != yuv420sp && m_frameFormat != yuv420i) return false;
#define ALWAYS_COPY_GRAY 0
#if ALWAYS_COPY_GRAY
    resmat.create(height, width, CV_8UC1);
    unsigned char* matBuff = resmat.ptr<unsigned char> (0);
    memcpy(matBuff, yuv, width * height);
#else
    resmat = cv::Mat(height, width, CV_8UC1, (void*)yuv);
#endif
    return !resmat.empty();
}

bool CvCapture_Android::convertYUV2BGR(int width, int height, const unsigned char* yuv, cv::Mat& resmat, bool inRGBorder, bool withAlpha)
{
    if (yuv == 0) return false;
    if (m_frameFormat != yuv420sp && m_frameFormat != yuv420i) return false;

    CV_Assert(width % 2 == 0 && height % 2 == 0);

    cv::Mat src(height*3/2, width, CV_8UC1, (void*)yuv);

    if (m_frameFormat == yuv420sp)
        cv::cvtColor(src, resmat, inRGBorder ? CV_YUV420sp2RGB : CV_YUV420sp2BGR, withAlpha ? 4 : 3);
    else if (m_frameFormat == yuv420i)
        cv::cvtColor(src, resmat, inRGBorder ? CV_YUV420i2RGB : CV_YUV420i2BGR, withAlpha ? 4 : 3);

    return !resmat.empty();
}

CvCapture* cvCreateCameraCapture_Android( int cameraId )
{
    CvCapture_Android* capture = new CvCapture_Android(cameraId);

    if( capture->isOpened() )
        return capture;

    delete capture;
    return 0;
}

#endif
