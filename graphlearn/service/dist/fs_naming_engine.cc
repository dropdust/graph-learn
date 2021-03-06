/* Copyright 2020 Alibaba Group Holding Limited. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <unistd.h>
#include <memory>
#include "graphlearn/common/base/errors.h"
#include "graphlearn/common/base/log.h"
#include "graphlearn/common/base/macros.h"
#include "graphlearn/common/string/numeric.h"
#include "graphlearn/common/string/string_tool.h"
#include "graphlearn/common/threading/sync/lock.h"
#include "graphlearn/platform/env.h"
#include "graphlearn/service/dist/naming_engine.h"

namespace graphlearn {

FSNamingEngine::FSNamingEngine()
    : NamingEngine(), stopping_(false), stopped_(false), fs_(nullptr) {
  if (strings::EndWith(GLOBAL_FLAG(Tracker), "/")) {
    tracker_ = GLOBAL_FLAG(Tracker) + "endpoints/";
  } else {
    tracker_ = GLOBAL_FLAG(Tracker) + "/endpoints/";
  }

  Status s = Env::Default()->GetFileSystem(tracker_, &fs_);
  if (!s.ok()) {
    USER_LOG("Invalid tracker path and exit now.");
    USER_LOG(tracker_);
    LOG(FATAL) << "Invalid tracker path: " << tracker_;
    ::exit(-1);
  }

  s = fs_->CreateDir(tracker_);
  if (s.ok() || error::IsAlreadyExists(s)) {
    LOG(INFO) << "Connect naming engine ok: " << tracker_;
  } else {
    USER_LOG("Connect to tracker path failed and exit now.");
    USER_LOG(tracker_);
    LOG(FATAL) << "Connect naming engine failed: " << tracker_;
    ::exit(-1);
  }

  endpoints_.resize(GLOBAL_FLAG(ServerCount));

  auto tp = Env::Default()->ReservedThreadPool();
  tp->AddTask(NewClosure(this, &FSNamingEngine::Refresh));
}

FSNamingEngine::~FSNamingEngine() {
  if (!stopped_) {
    Stop();
  }
}

Status FSNamingEngine::Update(int32_t server_id,
                            const std::string& endpoint) {
  std::string file_name = tracker_ + std::to_string(server_id);
  LOG(INFO) << "Update endpoint id: " << server_id
            << ", address: " << endpoint
            << ", filepath: " << file_name;

  std::unique_ptr<WritableFile> ret;
  Status s = fs_->NewWritableFile(file_name, &ret);
  RETURN_IF_NOT_OK(s)

  s = ret->Append(endpoint);
  RETURN_IF_NOT_OK(s)

  s = ret->Close();
  RETURN_IF_NOT_OK(s)
  return s;
}

void FSNamingEngine::Stop() {
  stopping_ = true;
  while (!stopped_) {
    usleep(1000);
  }
}

void FSNamingEngine::Refresh() {
  while (!stopping_) {
    std::vector<std::string> file_names;
    Status s = fs_->ListDir(tracker_, &file_names);
    if (s.ok()) {
      Parse(file_names);
    } else {
      LOG(WARNING) << "Refresh endpoints failed: " << s.ToString();
    }
    sleep(1);
  }
  stopped_ = true;
}

void FSNamingEngine::Parse(const std::vector<std::string>& names) {
  char buffer[32] = {0};

  int32_t count = 0;
  std::vector<std::string> tmp(endpoints_.size(), "");
  for (size_t i = 0; i < names.size(); ++i) {
    int32_t id = -1;
    if (!strings::SafeStringTo32(names[i], &id) ||
        id < 0 || id >= tmp.size()) {
      continue;
    }

    std::unique_ptr<ByteStreamAccessFile> ret;
    Status s = fs_->NewByteStreamAccessFile(tracker_ + names[i], 0, &ret);
    if (!s.ok()) {
      LOG(WARNING) << "Invalid endpoint file: " << names[i];
      continue;
    }

    LiteString endpoint;
    s = ret->Read(sizeof(buffer), &endpoint, buffer);
    if (s.ok()) {
      tmp[id] = endpoint.ToString();
      ++count;
    } else {
      LOG(WARNING) << "Invalid endpoint file: " << names[i];
    }
  }

  ScopedLocker<std::mutex> _(&mtx_);
  LOG(INFO) << "Refresh endpoints count: " << size_;
  size_ = count;
  endpoints_.swap(tmp);
}

}  // namespace graphlearn
