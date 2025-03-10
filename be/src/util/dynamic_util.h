// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/util/dynamic_util.h

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#pragma once

#include "common/status.h"

namespace starrocks {

// Look up smybols in a dynamically linked library.
// handle -- handle to the library. NULL if loading from the current process.
// symbol -- symbol to lookup.
// fn_ptr -- pointer tor retun addres of function.
Status dynamic_lookup(void* handle, const char* symbol, void** fn_ptr);

// Open a dynamicly loaded library.
// library -- name of the library.  The default paths will be searched.
//            library can be NULL to get the handle for the current process.
// handle -- returned handle to the library.
Status dynamic_open(const char* library, void** handle);

// Closes the handle.
void dynamic_close(void* handle);

} // namespace starrocks
