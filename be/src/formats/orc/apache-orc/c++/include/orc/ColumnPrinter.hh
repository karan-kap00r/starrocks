// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/orc/tree/main/c++/include/orc/ColumnPrinter.hh

/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "orc/OrcFile.hh"
#include "orc/Vector.hh"
#include "orc/orc-config.hh"

namespace orc {

class ColumnPrinter {
protected:
    std::string& buffer;
    bool hasNulls;
    const char* notNull;

public:
    ColumnPrinter(std::string&);
    virtual ~ColumnPrinter();
    virtual void printRow(uint64_t rowId) = 0;
    // should be called once at the start of each batch of rows
    virtual void reset(const ColumnVectorBatch& batch);
};

ORC_UNIQUE_PTR<ColumnPrinter> createColumnPrinter(std::string&, const Type* type);
} // namespace orc
