/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "velox/functions/lib/DateTimeFormatter.h"
#include "velox/functions/lib/TimeUtils.h"
#include "velox/functions/prestosql/DateTimeImpl.h"
#include "velox/type/tz/TimeZoneMap.h"

namespace facebook::velox::functions::sparksql {

template <typename T>
struct YearFunction : public InitSessionTimezone<T> {
  VELOX_DEFINE_FUNCTION_TYPES(T);

  FOLLY_ALWAYS_INLINE int32_t getYear(const std::tm& time) {
    return 1900 + time.tm_year;
  }

  FOLLY_ALWAYS_INLINE void call(
      int32_t& result,
      const arg_type<Timestamp>& timestamp) {
    result = getYear(getDateTime(timestamp, this->timeZone_));
  }

  FOLLY_ALWAYS_INLINE void call(int32_t& result, const arg_type<Date>& date) {
    result = getYear(getDateTime(date));
  }
};

template <typename T>
struct WeekFunction : public InitSessionTimezone<T> {
  VELOX_DEFINE_FUNCTION_TYPES(T);

  FOLLY_ALWAYS_INLINE int32_t getWeek(const std::tm& time) {
    // The computation of ISO week from date follows the algorithm here:
    // https://en.wikipedia.org/wiki/ISO_week_date
    int32_t week = floor(
                       10 + (time.tm_yday + 1) -
                       (time.tm_wday ? time.tm_wday : kDaysInWeek)) /
        kDaysInWeek;

    if (week == 0) {
      // Distance in days between the first day of the current year and the
      // Monday of the current week.
      auto mondayOfWeek =
          time.tm_yday + 1 - (time.tm_wday + kDaysInWeek - 1) % kDaysInWeek;
      // Distance in days between the first day and the first Monday of the
      // current year.
      auto firstMondayOfYear =
          1 + (mondayOfWeek + kDaysInWeek - 1) % kDaysInWeek;

      if ((util::isLeapYear(time.tm_year + 1900 - 1) &&
           firstMondayOfYear == 2) ||
          firstMondayOfYear == 3 || firstMondayOfYear == 4) {
        week = 53;
      } else {
        week = 52;
      }
    } else if (week == 53) {
      // Distance in days between the first day of the current year and the
      // Monday of the current week.
      auto mondayOfWeek =
          time.tm_yday + 1 - (time.tm_wday + kDaysInWeek - 1) % kDaysInWeek;
      auto daysInYear = util::isLeapYear(time.tm_year + 1900) ? 366 : 365;
      if (daysInYear - mondayOfWeek < 3) {
        week = 1;
      }
    }

    return week;
  }

  FOLLY_ALWAYS_INLINE void call(
      int32_t& result,
      const arg_type<Timestamp>& timestamp) {
    result = getWeek(getDateTime(timestamp, this->timeZone_));
  }

  FOLLY_ALWAYS_INLINE void call(int32_t& result, const arg_type<Date>& date) {
    result = getWeek(getDateTime(date));
  }
};

template <typename T>
struct UnixTimestampFunction {
  // unix_timestamp();
  // If no parameters, return the current unix timestamp without adjusting
  // timezones.
  FOLLY_ALWAYS_INLINE void call(int64_t& result) {
    result = Timestamp::now().getSeconds();
  }
};

template <typename T>
struct UnixTimestampParseFunction {
  VELOX_DEFINE_FUNCTION_TYPES(T);

  // unix_timestamp(input);
  // If format is not specified, assume kDefaultFormat.
  FOLLY_ALWAYS_INLINE void initialize(
      const core::QueryConfig& config,
      const arg_type<Varchar>* /*input*/) {
    format_ = buildJodaDateTimeFormatter(kDefaultFormat_);
    setTimezone(config);
  }

  FOLLY_ALWAYS_INLINE bool call(
      int64_t& result,
      const arg_type<Varchar>& input) {
    DateTimeResult dateTimeResult;
    try {
      dateTimeResult =
          format_->parse(std::string_view(input.data(), input.size()));
    } catch (const VeloxUserError&) {
      // Return null if could not parse.
      return false;
    }
    dateTimeResult.timestamp.toGMT(getTimezoneId(dateTimeResult));
    result = dateTimeResult.timestamp.getSeconds();
    return true;
  }

 protected:
  void setTimezone(const core::QueryConfig& config) {
    auto sessionTzName = config.sessionTimezone();
    if (!sessionTzName.empty()) {
      sessionTzID_ = util::getTimeZoneID(sessionTzName);
    }
  }

  int16_t getTimezoneId(const DateTimeResult& result) {
    // If timezone was not parsed, fallback to the session timezone. If there's
    // no session timezone, fallback to 0 (GMT).
    return result.timezoneId != -1 ? result.timezoneId
                                   : sessionTzID_.value_or(0);
  }

  // Default if format is not specified, as per Spark documentation.
  constexpr static std::string_view kDefaultFormat_{"yyyy-MM-dd HH:mm:ss"};
  std::shared_ptr<DateTimeFormatter> format_;
  std::optional<int64_t> sessionTzID_;
};

template <typename T>
struct UnixTimestampParseWithFormatFunction
    : public UnixTimestampParseFunction<T> {
  VELOX_DEFINE_FUNCTION_TYPES(T);

  // unix_timestamp(input, format):
  // If format is constant, compile it just once per batch.
  FOLLY_ALWAYS_INLINE void initialize(
      const core::QueryConfig& config,
      const arg_type<Varchar>* /*input*/,
      const arg_type<Varchar>* format) {
    if (format != nullptr) {
      try {
        this->format_ = buildJodaDateTimeFormatter(
            std::string_view(format->data(), format->size()));
      } catch (const VeloxUserError&) {
        invalidFormat_ = true;
      }
      isConstFormat_ = true;
    }
    this->setTimezone(config);
  }

  FOLLY_ALWAYS_INLINE bool call(
      int64_t& result,
      const arg_type<Varchar>& input,
      const arg_type<Varchar>& format) {
    if (invalidFormat_) {
      return false;
    }

    // Format or parsing error returns null.
    try {
      if (!isConstFormat_) {
        this->format_ = buildJodaDateTimeFormatter(
            std::string_view(format.data(), format.size()));
      }

      auto dateTimeResult =
          this->format_->parse(std::string_view(input.data(), input.size()));
      dateTimeResult.timestamp.toGMT(this->getTimezoneId(dateTimeResult));
      result = dateTimeResult.timestamp.getSeconds();
    } catch (const VeloxUserError&) {
      return false;
    }
    return true;
  }

 private:
  bool isConstFormat_{false};
  bool invalidFormat_{false};
};

template <typename T>
struct MakeDateFunction {
  VELOX_DEFINE_FUNCTION_TYPES(T);

  FOLLY_ALWAYS_INLINE void call(
      out_type<Date>& result,
      const int32_t year,
      const int32_t month,
      const int32_t day) {
    auto daysSinceEpoch = util::daysSinceEpochFromDate(year, month, day);
    VELOX_USER_CHECK_EQ(
        daysSinceEpoch,
        (int32_t)daysSinceEpoch,
        "Integer overflow in make_date({}, {}, {})",
        year,
        month,
        day);
    result = daysSinceEpoch;
  }
};

template <typename T>
struct LastDayFunction {
  VELOX_DEFINE_FUNCTION_TYPES(T);

  FOLLY_ALWAYS_INLINE int64_t getYear(const std::tm& time) {
    return 1900 + time.tm_year;
  }

  FOLLY_ALWAYS_INLINE int64_t getMonth(const std::tm& time) {
    return 1 + time.tm_mon;
  }

  FOLLY_ALWAYS_INLINE int64_t getDay(const std::tm& time) {
    return time.tm_mday;
  }

  FOLLY_ALWAYS_INLINE void call(
      out_type<Date>& result,
      const arg_type<Date>& date) {
    auto dateTime = getDateTime(date);
    int32_t year = getYear(dateTime);
    int32_t month = getMonth(dateTime);
    int32_t day = getMonth(dateTime);
    auto lastDay = util::getMaxDayOfMonth(year, month);
    auto daysSinceEpoch = util::daysSinceEpochFromDate(year, month, lastDay);
    VELOX_USER_CHECK_EQ(
        daysSinceEpoch,
        (int32_t)daysSinceEpoch,
        "Integer overflow in last_day({}-{}-{})",
        year,
        month,
        day);
    result = daysSinceEpoch;
  }
};

template <typename T>
struct DateAddFunction {
  VELOX_DEFINE_FUNCTION_TYPES(T);

  FOLLY_ALWAYS_INLINE void call(
      out_type<Date>& result,
      const arg_type<Date>& date,
      const int32_t value) {
    result = addToDate(date, DateTimeUnit::kDay, value);
  }
};

template <typename T>
struct DateSubFunction {
  VELOX_DEFINE_FUNCTION_TYPES(T);

  FOLLY_ALWAYS_INLINE void call(
      out_type<Date>& result,
      const arg_type<Date>& date,
      const int32_t value) {
    constexpr int32_t kMin = std::numeric_limits<int32_t>::min();
    if (value > kMin) {
      int32_t subValue = 0 - value;
      result = addToDate(date, DateTimeUnit::kDay, subValue);
    } else {
      // If input values is kMin,  0 - value overflows.
      // Subtract kMin in 2 steps to avoid overflow: -(-(kMin+1)), then -1.
      int32_t subValue = 0 - (kMin + 1);
      result = addToDate(date, DateTimeUnit::kDay, subValue);
      result = addToDate(result, DateTimeUnit::kDay, 1);
    }
  }
};

template <typename T>
struct DayOfWeekFunction : public InitSessionTimezone<T>,
                           public TimestampWithTimezoneSupport<T> {
  VELOX_DEFINE_FUNCTION_TYPES(T);

  // 1 = Sunday, 2 = Monday, ..., 7 = Saturday
  FOLLY_ALWAYS_INLINE int32_t getDayOfWeek(const std::tm& time) {
    return time.tm_wday + 1;
  }

  FOLLY_ALWAYS_INLINE void call(
      int32_t& result,
      const arg_type<Timestamp>& timestamp) {
    result = getDayOfWeek(getDateTime(timestamp, this->timeZone_));
  }

  FOLLY_ALWAYS_INLINE void call(int32_t& result, const arg_type<Date>& date) {
    result = getDayOfWeek(getDateTime(date));
  }
};

template <typename T>
struct DateDiffFunction {
  VELOX_DEFINE_FUNCTION_TYPES(T);

  FOLLY_ALWAYS_INLINE void call(
      int32_t& result,
      const arg_type<Date>& endDate,
      const arg_type<Date>& startDate)
#if defined(__has_feature)
#if __has_feature(__address_sanitizer__)
      __attribute__((__no_sanitize__("signed-integer-overflow")))
#endif
#endif
  {
    result = endDate - startDate;
  }
};

template <typename T>
struct AddMonthsFunction {
  VELOX_DEFINE_FUNCTION_TYPES(T);

  FOLLY_ALWAYS_INLINE void
  call(out_type<Date>& result, const arg_type<Date>& date, int32_t numMonths) {
    const auto dateTime = getDateTime(date);
    const auto year = getYear(dateTime);
    const auto month = getMonth(dateTime);
    const auto day = getDay(dateTime);

    // Similar to handling number in base 12. Here, month - 1 makes it in
    // [0, 11] range.
    int64_t monthAdded = (int64_t)month - 1 + numMonths;
    // Used to adjust month/year when monthAdded is not in [0, 11] range.
    int64_t yearOffset = (monthAdded >= 0 ? monthAdded : monthAdded - 11) / 12;
    // Adjusts monthAdded to natural month number in [1, 12] range.
    auto monthResult = static_cast<int32_t>(monthAdded - yearOffset * 12 + 1);
    // Adjusts year.
    auto yearResult = year + yearOffset;

    auto lastDayOfMonth = util::getMaxDayOfMonth(yearResult, monthResult);
    // Adjusts day to valid one.
    auto dayResult = lastDayOfMonth < day ? lastDayOfMonth : day;
    auto daysSinceEpoch =
        util::daysSinceEpochFromDate(yearResult, monthResult, dayResult);
    VELOX_USER_CHECK_EQ(
        daysSinceEpoch,
        (int32_t)daysSinceEpoch,
        "Integer overflow in add_months({}, {})",
        DATE()->toString(date),
        numMonths);
    result = daysSinceEpoch;
  }
};
} // namespace facebook::velox::functions::sparksql
