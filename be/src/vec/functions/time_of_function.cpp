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

#include "vec/data_types/data_type_number.h"
#include "vec/functions/date_time_transforms.h"
#include "vec/functions/function_date_or_datetime_to_something.h"
#include "vec/functions/simple_function_factory.h"

namespace doris::vectorized {

using FunctionWeekOfYear = FunctionDateOrDateTimeToSomething<DataTypeInt32, WeekOfYearImpl>;
using FunctionDayOfYear = FunctionDateOrDateTimeToSomething<DataTypeInt32, DayOfYearImpl>;
using FunctionDayOfWeek = FunctionDateOrDateTimeToSomething<DataTypeInt32, DayOfWeekImpl>;
using FunctionDayOfMonth = FunctionDateOrDateTimeToSomething<DataTypeInt32, DayOfMonthImpl>;
using FunctionYearWeek = FunctionDateOrDateTimeToSomething<DataTypeInt32, ToYearWeekOneArgImpl>;

void register_function_time_of_fuction(SimpleFunctionFactory& factory) {
    factory.register_function<FunctionDayOfWeek>();
    factory.register_function<FunctionDayOfMonth>();
    factory.register_function<FunctionDayOfYear>();
    factory.register_function<FunctionWeekOfYear>();
    factory.register_function<FunctionYearWeek>();
}
} // namespace doris::vectorized