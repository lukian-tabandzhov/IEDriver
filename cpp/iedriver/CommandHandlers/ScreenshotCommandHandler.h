// Copyright 2011 Software Freedom Conservancy
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef WEBDRIVER_IE_SCREENSHOTCOMMANDHANDLER_H_
#define WEBDRIVER_IE_SCREENSHOTCOMMANDHANDLER_H_

#include "../Browser.h"
#include "../IECommandHandler.h"
#include "../IECommandExecutor.h"
#include "logging.h"
#include <atlimage.h>
#include <atlenc.h>

// Define a shared data segment.  Variables in this segment can be
// shared across processes that load this DLL.
#pragma data_seg("SHARED")
HHOOK next_hook = NULL;
HWND ie_window_handle = NULL;
int max_width = 0;
int max_height = 0;
#pragma data_seg()

#pragma comment(linker, "/section:SHARED,RWS")

namespace webdriver {

class ScreenshotCommandHandler : public IECommandHandler {
 public:
  ScreenshotCommandHandler(void) {
    this->image_ = NULL;
  }

  virtual ~ScreenshotCommandHandler(void) {
  }

 protected:
  void ExecuteInternal(const IECommandExecutor& executor,
                       const ParametersMap& command_parameters,
                       Response* response) {
    LOG(TRACE) << "Entering ScreenshotCommandHandler::ExecuteInternal";

    BrowserHandle browser_wrapper;
    int status_code = executor.GetCurrentBrowser(&browser_wrapper);
    if (status_code != WD_SUCCESS) {
      response->SetErrorResponse(status_code, "Unable to get browser");
      return;
    }

    bool isSameColour = true;
    HRESULT hr;
    int i = 0;
    int tries = 4;
    do {
      this->ClearImage();

      this->image_ = new CImage();
      hr = this->CaptureBrowser(browser_wrapper);
      if (FAILED(hr)) {
        LOGHR(WARN, hr) << "Failed to capture browser image at " << i << " try";
        this->ClearImage();
        response->SetSuccessResponse("");
        return;
      }

      isSameColour = IsSameColour();
      if (isSameColour) {
        ::Sleep(2000);
        LOG(DEBUG) << "Failed to capture non single color browser image at " << i << " try";
      }

      i++;
    } while ((i < tries) && isSameColour);

    // now either correct or single color image is got
    std::string base64_screenshot = "";
    hr = this->GetBase64Data(base64_screenshot);
    if (FAILED(hr)) {
      LOGHR(WARN, hr) << "Unable to transform browser image to Base64 format";
      this->ClearImage();
      response->SetSuccessResponse("");
      return;
    }

    this->ClearImage();
    response->SetSuccessResponse(base64_screenshot);
  }

 private:
  ATL::CImage* image_;

  void ClearImage() {
    if (this->image_ != NULL) {
      delete this->image_;
      this->image_ = NULL;
    }
  }

  HRESULT CaptureBrowser(BrowserHandle browser) {
    LOG(TRACE) << "Entering ScreenshotCommandHandler::CaptureBrowser";

    HWND content_window_handle = browser->GetWindowHandle();
    int content_width = 0, content_height = 0;

    this->GetWindowDimensions(content_window_handle, &content_width, &content_height);

    // Capture the window's canvas to a DIB.
    BOOL created = this->image_->Create(content_width,
                                        content_height,
                                        /*numbers of bits per pixel = */ 32);
    if (!created) {
      LOG(WARN) << "Unable to initialize image object";
    }
    HDC device_context_handle = this->image_->GetDC();

    BOOL print_result = ::PrintWindow(content_window_handle,
                                      device_context_handle,
                                      PW_CLIENTONLY);
    if (!print_result) {
      LOG(WARN) << "PrintWindow API is not able to get content window screenshot";
    }

    this->image_->ReleaseDC();
    return S_OK;
  }

  bool IsSameColour() {
    COLORREF firstPixelColour = this->image_->GetPixel(0, 0);

    for (int i = 0; i < this->image_->GetWidth(); i++) {
      for (int j = 0; j < this->image_->GetHeight(); j++) {
        if (firstPixelColour != this->image_->GetPixel(i, j)) {
          return false;
        }
      }
    }

    return true;
  }

  HRESULT GetBase64Data(std::string& data) {
    LOG(TRACE) << "Entering ScreenshotCommandHandler::GetBase64Data";

    if (this->image_ == NULL) {
      LOG(DEBUG) << "CImage was not initialized.";
      return E_POINTER;
    }

    CComPtr<IStream> stream;
    HRESULT hr = ::CreateStreamOnHGlobal(NULL, TRUE, &stream);
    if (FAILED(hr)) {
      LOGHR(WARN, hr) << "Error is occured during creating IStream";
      return hr;
    }

    GUID image_format = Gdiplus::ImageFormatPNG /*Gdiplus::ImageFormatJPEG*/;
    hr = this->image_->Save(stream, image_format);
    if (FAILED(hr)) {
      LOGHR(WARN, hr) << "Saving screenshot image is failed";
      return hr;
    }

    // Get the size of the stream.
    STATSTG statstg;
    hr = stream->Stat(&statstg, STATFLAG_DEFAULT);
    if (FAILED(hr)) {
      LOGHR(WARN, hr) << "No stat on stream is got";
      return hr;
    }

    HGLOBAL global_memory_handle = NULL;
    hr = ::GetHGlobalFromStream(stream, &global_memory_handle);
    if (FAILED(hr)) {
      LOGHR(WARN, hr) << "No HGlobal in stream";
      return hr;
    }

    // TODO: What if the file is bigger than max_int?
    int stream_size = static_cast<int>(statstg.cbSize.QuadPart);
    LOG(DEBUG) << "Size of screenshot image stream is " << stream_size;

    int length = ::Base64EncodeGetRequiredLength(stream_size, ATL_BASE64_FLAG_NOCRLF);
    if (length <= 0) {
      LOG(WARN) << "Got zero or negative length from base64 required length";
      return E_FAIL;
    }

    BYTE* global_lock = reinterpret_cast<BYTE*>(::GlobalLock(global_memory_handle));
    if (global_lock == NULL) {
      LOGERR(WARN) << "Unable to lock memory for base64 encoding";
      ::GlobalUnlock(global_memory_handle);      
      return E_FAIL;
    }

    char* data_array = new char[length + 1];
    if (!::Base64Encode(global_lock,
                        stream_size,
                        data_array,
                        &length,
                        ATL_BASE64_FLAG_NOCRLF)) {
      delete[] data_array;
      ::GlobalUnlock(global_memory_handle);
      LOG(WARN) << "Unable to encode image stream to base64";
      return E_FAIL;
    }
    data_array[length] = '\0';
    data = data_array;

    delete[] data_array;
    ::GlobalUnlock(global_memory_handle);

    return S_OK;
  }

  void GetBrowserChromeDimensions(HWND top_level_window_handle,
                                  HWND content_window_handle,
                                  int* width,
                                  int* height) {
    LOG(TRACE) << "Entering ScreenshotCommandHandler::GetBrowserChromeDimensions";

    int top_level_window_width = 0;
    int top_level_window_height = 0;
    this->GetWindowDimensions(top_level_window_handle,
                              &top_level_window_width,
                              &top_level_window_height);
    LOG(TRACE) << "Top level window dimensions are (w, h): "
               << top_level_window_width << "," << top_level_window_height;

    int content_window_width = 0;
    int content_window_height = 0;
    this->GetWindowDimensions(content_window_handle,
                              &content_window_width,
                              &content_window_height);
    LOG(TRACE) << "Content window dimensions are (w, h): "
               << content_window_width << "," << content_window_height;

    *width = top_level_window_width - content_window_width;
    *height = top_level_window_height - content_window_height;
  }

  void GetWindowDimensions(HWND window_handle, int* width, int* height) {
    RECT window_rect;
    ::GetWindowRect(window_handle, &window_rect);
    *width = window_rect.right - window_rect.left;
    *height = window_rect.bottom - window_rect.top;
  }
};

} // namespace webdriver

#endif // WEBDRIVER_IE_SCREENSHOTCOMMANDHANDLER_H_
