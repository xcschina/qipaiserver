/****************************************************************************
 Copyright (c) 2012      greathqy
 Copyright (c) 2012      cocos2d-x.org
 Copyright (c) 2013-2014 Chukong Technologies Inc.
 
 http://www.cocos2d-x.org
 
 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:
 
 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.
 
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
 ****************************************************************************/

#include "HttpClientEx.h"

#include <thread>
#include <queue>
#include <condition_variable>

#include <errno.h>

#include "curl/curl.h"
#pragma comment(lib,"curl/libcurl_imp.lib")

namespace network {
    
    static std::mutex       s_requestQueueMutex;
    static std::mutex       s_responseQueueMutex;
    
    static std::mutex       s_SleepMutex;
    static std::condition_variable      s_SleepCondition;
    
    
    typedef int int32_t;
    
    static bool s_need_quit = false;
    
    static std::vector<HttpRequestEx*>*  s_requestQueue = nullptr;
    static std::vector<HttpResponseEx*>* s_responseQueue = nullptr;
    
    static HttpClientEx *s_pHttpClient = nullptr; // pointer to singleton
    
    static char s_errorBuffer[1024] = {0};
    
    typedef size_t (*write_callback)(void *ptr, size_t size, size_t nmemb, void *stream);
    
    static std::string s_cookieFilename = "";
    
    // Callback function used by libcurl for collect response data
    static size_t writeData(void *ptr, size_t size, size_t nmemb, void *stream)
    {
        std::vector<char> *recvBuffer = (std::vector<char>*)stream;
        size_t sizes = size * nmemb;
        
        // add data to the end of recvBuffer
        // write data maybe called more than once in a single request
        recvBuffer->insert(recvBuffer->end(), (char*)ptr, (char*)ptr+sizes);
        
        return sizes;
    }
    
    // Callback function used by libcurl for collect header data
    static size_t writeHeaderData(void *ptr, size_t size, size_t nmemb, void *stream)
    {
        std::vector<char> *recvBuffer = (std::vector<char>*)stream;
        size_t sizes = size * nmemb;
        
        // add data to the end of recvBuffer
        // write data maybe called more than once in a single request
        recvBuffer->insert(recvBuffer->end(), (char*)ptr, (char*)ptr+sizes);
        
        return sizes;
    }
    
    
    static int processGetTask(HttpRequestEx *request, write_callback callback, void *stream, long *errorCode, write_callback headerCallback, void *headerStream, char *errorBuffer);
    static int processPostTask(HttpRequestEx *request, write_callback callback, void *stream, long *errorCode, write_callback headerCallback, void *headerStream, char *errorBuffer);
    static int processPutTask(HttpRequestEx *request, write_callback callback, void *stream, long *errorCode, write_callback headerCallback, void *headerStream, char *errorBuffer);
    static int processDeleteTask(HttpRequestEx *request, write_callback callback, void *stream, long *errorCode, write_callback headerCallback, void *headerStream, char *errorBuffer);
    static int processPostFileTask(HttpRequestEx *request, write_callback callback, void *stream, long *responseCode, write_callback headerCallback, void *headerStream, char *errorBuffer);
    // int processDownloadTask(HttpRequest *task, write_callback callback, void *stream, int32_t *errorCode);
    static void processResponse(HttpResponseEx* response, char* errorBuffer);
    
    // Worker thread
    void HttpClientEx::networkThread()
    {
        HttpRequestEx *request = nullptr;
        
        auto scheduler = Director::getInstance()->getScheduler();
        
        while (true)
        {
            if (s_need_quit)
            {
                break;
            }
            
            // step 1: send http request if the requestQueue isn't empty
            request = nullptr;
            
            s_requestQueueMutex.lock();
            
            //Get request task from queue
            
            if (!s_requestQueue->empty())
            {
                request = s_requestQueue->at(0);
                s_requestQueue->erase(0);
            }
            
            s_requestQueueMutex.unlock();
            
            if (nullptr == request)
            {
                // Wait for http request tasks from main thread
                std::unique_lock<std::mutex> lk(s_SleepMutex);
                s_SleepCondition.wait(lk);
                continue;
            }
            
            // step 2: libcurl sync access
            
            // Create a HttpResponse object, the default setting is http access failed
            HttpResponseEx *response = new HttpResponseEx(request);
            
            processResponse(response, s_errorBuffer);
            
            
            // add response packet into queue
            s_responseQueueMutex.lock();
            s_responseQueue->pushBack(response);
            s_responseQueueMutex.unlock();
            
            if (nullptr != s_pHttpClient) {
                scheduler->performFunctionInCocosThread(CC_CALLBACK_0(HttpClientEx::dispatchResponseCallbacks, this));
            }
        }
        
        // cleanup: if worker thread received quit signal, clean up un-completed request queue
        s_requestQueueMutex.lock();
        s_requestQueue->clear();
        s_requestQueueMutex.unlock();
        
        
        if (s_requestQueue != nullptr) {
            delete s_requestQueue;
            s_requestQueue = nullptr;
            delete s_responseQueue;
            s_responseQueue = nullptr;
        }
        
    }
    
    // Worker thread
    void HttpClientEx::networkThreadAlone(HttpRequestEx* request)
    {
        // Create a HttpResponse object, the default setting is http access failed
        HttpResponseEx *response = new HttpResponseEx(request);
        char errorBuffer[CURL_ERROR_SIZE] = { 0 };
        processResponse(response, errorBuffer);
        
        auto scheduler = Director::getInstance()->getScheduler();
        scheduler->performFunctionInCocosThread([response, request]{
            const ccHttpRequestCallback& callback = request->getCallback();
            Ref* pTarget = request->getTarget();
            SEL_HttpResponse pSelector = request->getSelector();
            
            if (callback != nullptr)
            {
                callback(s_pHttpClient, response);
            }
            else if (pTarget && pSelector)
            {
                (pTarget->*pSelector)(s_pHttpClient, response);
            }
            response->release();
            // do not release in other thread
            request->release();
        });
    }
    
    //Configure curl's timeout property
    static bool configureCURL(CURL *handle, char *errorBuffer)
    {
        if (!handle) {
            return false;
        }
        
        int32_t code;
        code = curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, errorBuffer);
        if (code != CURLE_OK) {
            return false;
        }
        code = curl_easy_setopt(handle, CURLOPT_TIMEOUT, HttpClientEx::getInstance()->getTimeoutForRead());
        if (code != CURLE_OK) {
            return false;
        }
        code = curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, HttpClientEx::getInstance()->getTimeoutForConnect());
        if (code != CURLE_OK) {
            return false;
        }
        curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 0L);
        
        // FIXED #3224: The subthread of CCHttpClient interrupts main thread if timeout comes.
        // Document is here: http://curl.haxx.se/libcurl/c/curl_easy_setopt.html#CURLOPTNOSIGNAL
        curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
        
        return true;
    }
    
    class CURLRaii
    {
        /// Instance of CURL
        CURL *_curl;
        /// Keeps custom header data
        curl_slist *_headers;
    public:
        CURLRaii()
        : _curl(curl_easy_init())
        , _headers(nullptr)
        {
        }
        
        ~CURLRaii()
        {
            if (_curl)
                curl_easy_cleanup(_curl);
            /* free the linked list for header data */
            if (_headers)
                curl_slist_free_all(_headers);
        }
        
        template <class T>
        bool setOption(CURLoption option, T data)
        {
            return CURLE_OK == curl_easy_setopt(_curl, option, data);
        }
        
        /**
         * @brief Inits CURL instance for common usage
         * @param request Null not allowed
         * @param callback Response write callback
         * @param stream Response write stream
         */
        bool init(HttpRequestEx *request, write_callback callback, void *stream, write_callback headerCallback, void *headerStream, char *errorBuffer)
        {
            if (!_curl)
                return false;
            if (!configureCURL(_curl, errorBuffer))
                return false;
            
            /* get custom header data (if set) */
            std::vector<std::string> headers=request->getHeaders();
            if(!headers.empty())
            {
                /* append custom headers one by one */
                for (std::vector<std::string>::iterator it = headers.begin(); it != headers.end(); ++it)
                    _headers = curl_slist_append(_headers,it->c_str());
                /* set custom headers for curl */
                if (!setOption(CURLOPT_HTTPHEADER, _headers))
                    return false;
            }
            if (!s_cookieFilename.empty()) {
                if (!setOption(CURLOPT_COOKIEFILE, s_cookieFilename.c_str())) {
                    return false;
                }
                if (!setOption(CURLOPT_COOKIEJAR, s_cookieFilename.c_str())) {
                    return false;
                }
            }
            
            return setOption(CURLOPT_URL, request->getUrl())
            && setOption(CURLOPT_WRITEFUNCTION, callback)
            && setOption(CURLOPT_WRITEDATA, stream)
            && setOption(CURLOPT_HEADERFUNCTION, headerCallback)
            && setOption(CURLOPT_HEADERDATA, headerStream);
            
        }
        
        /// @param responseCode Null not allowed
        bool perform(long *responseCode)
        {
            if (CURLE_OK != curl_easy_perform(_curl))
                return false;
            CURLcode code = curl_easy_getinfo(_curl, CURLINFO_RESPONSE_CODE, responseCode);
            if (code != CURLE_OK || !(*responseCode >= 200 && *responseCode < 300)) {
                CCLOGERROR("Curl curl_easy_getinfo failed: %s", curl_easy_strerror(code));
                return false;
            }
            // Get some mor data.
            
            return true;
        }
    };
    
    //Process Get Request
    static int processGetTask(HttpRequestEx *request, write_callback callback, void *stream, long *responseCode, write_callback headerCallback, void *headerStream, char *errorBuffer)
    {
        CURLRaii curl;
        bool ok = curl.init(request, callback, stream, headerCallback, headerStream, errorBuffer)
        && curl.setOption(CURLOPT_FOLLOWLOCATION, true)
        && curl.perform(responseCode);
        return ok ? 0 : 1;
    }
    
    //Process POST Request
    static int processPostTask(HttpRequestEx *request, write_callback callback, void *stream, long *responseCode, write_callback headerCallback, void *headerStream, char *errorBuffer)
    {
        CURLRaii curl;
        bool ok = curl.init(request, callback, stream, headerCallback, headerStream, errorBuffer)
        && curl.setOption(CURLOPT_POST, 1)
        && curl.setOption(CURLOPT_POSTFIELDS, request->getRequestData())
        && curl.setOption(CURLOPT_POSTFIELDSIZE, request->getRequestDataSize())
        && curl.perform(responseCode);
        return ok ? 0 : 1;
    }
    
    //Process PUT Request
    static int processPutTask(HttpRequestEx *request, write_callback callback, void *stream, long *responseCode, write_callback headerCallback, void *headerStream, char *errorBuffer)
    {
        CURLRaii curl;
        bool ok = curl.init(request, callback, stream, headerCallback, headerStream, errorBuffer)
        && curl.setOption(CURLOPT_CUSTOMREQUEST, "PUT")
        && curl.setOption(CURLOPT_POSTFIELDS, request->getRequestData())
        && curl.setOption(CURLOPT_POSTFIELDSIZE, request->getRequestDataSize())
        && curl.perform(responseCode);
        return ok ? 0 : 1;
    }
    
    //Process DELETE Request
    static int processDeleteTask(HttpRequestEx *request, write_callback callback, void *stream, long *responseCode, write_callback headerCallback, void *headerStream, char *errorBuffer)
    {
        CURLRaii curl;
        bool ok = curl.init(request, callback, stream, headerCallback, headerStream, errorBuffer)
        && curl.setOption(CURLOPT_CUSTOMREQUEST, "DELETE")
        && curl.setOption(CURLOPT_FOLLOWLOCATION, true)
        && curl.perform(responseCode);
        return ok ? 0 : 1;
    }
    
    static int processPostFileTask(HttpRequestEx *request, write_callback callback, void *stream, long *responseCode, write_callback headerCallback, void *headerStream, char *errorBuffer)
    {
        std::vector<std::string> headers;
        headers.push_back("Expect:");
        request->setHeaders(headers);
        
        struct curl_httppost *formpost = NULL;
        struct curl_httppost *lastptr = NULL;
        
        std::map<std::string, std::string> requestParams = request->getFileData();
        
        curl_formadd(&formpost,
                     &lastptr,
                     CURLFORM_COPYNAME, requestParams.find("fileName")->second.c_str(),
                     CURLFORM_FILE,  requestParams.find("filePath")->second.c_str(),
                     CURLFORM_CONTENTTYPE, requestParams.find("contentType")->second.c_str(),
                     CURLFORM_END);
        
        //if( requestParams.find("content") != requestParams.end()){
        //    curl_formadd(&formpost,
        //                 &lastptr,
        //                 CURLFORM_PTRNAME, "content",
        //                 CURLFORM_PTRCONTENTS, requestParams.find("content")->second.c_str(),
        //                 CURLFORM_END);
        //}

		struct curl_httppost *tempformpost = formpost;
		typedef std::map<std::string, std::string>::iterator it_type;
		for(it_type iterator = requestParams.begin(); iterator != requestParams.end(); iterator++) {
			std::string key = iterator->first;
			std::string value = iterator->second;
			int Temp = key.size();
			char* pCharkey = new char[key.size()+1];
			char* pCharValue = new char[value.size()+1];
			sprintf(pCharkey,"%s",key.c_str());
			sprintf(pCharValue,"%s",value.c_str());
			curl_formadd(&formpost,
				&lastptr,
				CURLFORM_PTRNAME, pCharkey,
				CURLFORM_PTRCONTENTS, pCharValue,
				CURLFORM_END);
		}
        
        CURLRaii curl;
        bool ok = curl.init(request, callback, stream, headerCallback, headerStream, errorBuffer)
        && curl.setOption(CURLOPT_POST, 1)
        && curl.setOption(CURLOPT_HTTPPOST, formpost)
        && curl.perform(responseCode);
        
        curl_formfree(formpost);
        return ok ? 0 : 1;
    }
    
    // Process Response
    static void processResponse(HttpResponseEx* response, char* errorBuffer)
    {
        auto request = response->getHttpRequest();
        long responseCode = -1;
        int retValue = 0;
        
        // Process the request -> get response packet
        switch (request->getRequestType())
        {
            case HttpRequestEx::Type::GET: // HTTP GET
                retValue = processGetTask(request,
                                          writeData,
                                          response->getResponseData(),
                                          &responseCode,
                                          writeHeaderData,
                                          response->getResponseHeader(),
                                          errorBuffer);
                break;
                
            case HttpRequestEx::Type::POST: // HTTP POST
                retValue = processPostTask(request,
                                           writeData,
                                           response->getResponseData(),
                                           &responseCode,
                                           writeHeaderData,
                                           response->getResponseHeader(),
                                           errorBuffer);
                break;
                
            case HttpRequestEx::Type::PUT:
                retValue = processPutTask(request,
                                          writeData,
                                          response->getResponseData(),
                                          &responseCode,
                                          writeHeaderData,
                                          response->getResponseHeader(),
                                          errorBuffer);
                break;
                
            case HttpRequestEx::Type::DELETE:
                retValue = processDeleteTask(request,
                                             writeData,
                                             response->getResponseData(),
                                             &responseCode,
                                             writeHeaderData,
                                             response->getResponseHeader(),
                                             errorBuffer);
                break;
                
            case HttpRequestEx::Type::PostFile:
                retValue = processPostFileTask(request,
                                               writeData,
                                               response->getResponseData(),
                                               &responseCode,
                                               writeHeaderData,
                                               response->getResponseHeader(), errorBuffer);
            default:
                CCASSERT(true, "CCHttpClient: unkown request type, only GET and POSt are supported");
                break;
        }
        
        // write data to HttpResponse
        response->setResponseCode(responseCode);
        
        if (retValue != 0)
        {
            response->setSucceed(false);
            response->setErrorBuffer(errorBuffer);
        }
        else
        {
            response->setSucceed(true);
        }
    }
    
    // HttpClient implementation
    HttpClientEx* HttpClientEx::getInstance()
    {
        if (s_pHttpClient == nullptr) {
            s_pHttpClient = new HttpClientEx();
        }
        
        return s_pHttpClient;
    }
    
    void HttpClientEx::destroyInstance()
    {
        CC_SAFE_DELETE(s_pHttpClient);
    }
    
    void HttpClientEx::enableCookies(const char* cookieFile) {
        if (cookieFile) {
            s_cookieFilename = std::string(cookieFile);
        }
        else {
            s_cookieFilename = (FileUtils::getInstance()->getWritablePath() + "cookieFile.txt");
        }
    }
    
    HttpClientEx::HttpClientEx()
    : _timeoutForConnect(30)
    , _timeoutForRead(60)
    {
    }
    
    HttpClientEx::~HttpClientEx()
    {
        s_need_quit = true;
        
        if (s_requestQueue != nullptr) {
            s_SleepCondition.notify_one();
        }
        
        s_pHttpClient = nullptr;
    }
    
    //Lazy create semaphore & mutex & thread
    bool HttpClientEx::lazyInitThreadSemphore()
    {
        if (s_requestQueue != nullptr) {
            return true;
        } else {
            
            s_requestQueue = new Vector<HttpRequestEx*>();
            s_responseQueue = new Vector<HttpResponseEx*>();
            
            s_need_quit = false;
            
            auto t = std::thread(CC_CALLBACK_0(HttpClientEx::networkThread, this));
            t.detach();
        }
        
        return true;
    }
    
    //Add a get task to queue
    void HttpClientEx::send(HttpRequestEx* request)
    {    
        if (false == lazyInitThreadSemphore()) 
        {
            return;
        }
        
        if (!request)
        {
            return;
        }
        
        request->retain();
        
        if (nullptr != s_requestQueue) {
            s_requestQueueMutex.lock();
            s_requestQueue->pushBack(request);
            s_requestQueueMutex.unlock();
            
            // Notify thread start to work
            s_SleepCondition.notify_one();
        }
    }
    
    void HttpClientEx::sendImmediate(HttpRequestEx* request)
    {
        if(!request)
        {
            return;
        }
        
        request->retain();
        auto t = std::thread(&HttpClientEx::networkThreadAlone, this, request);
        t.detach();
    }
    
    // Poll and notify main thread if responses exists in queue
    void HttpClientEx::dispatchResponseCallbacks()
    {
        // log("CCHttpClient::dispatchResponseCallbacks is running");
        //occurs when cocos thread fires but the network thread has already quited
        if (nullptr == s_responseQueue) {
            return;
        }
        HttpResponseEx* response = nullptr;
        
        s_responseQueueMutex.lock();
        
        if (!s_responseQueue->empty())
        {
            response = s_responseQueue->at(0);
            s_responseQueue->erase(0);
        }
        
        s_responseQueueMutex.unlock();
        
        if (response)
        {
            HttpRequestEx *request = response->getHttpRequest();
            const ccHttpRequestCallback& callback = request->getCallback();
            Ref* pTarget = request->getTarget();
            SEL_HttpResponse pSelector = request->getSelector();
            
            if (callback != nullptr)
            {
                callback(this, response);
            }
            else if (pTarget && pSelector)
            {
                (pTarget->*pSelector)(this, response);
            }
            
            response->release();
            // do not release in other thread
            request->release();
        }
    }
    
}

NS_CC_END


