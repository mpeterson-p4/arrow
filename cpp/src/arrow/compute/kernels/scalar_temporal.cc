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

#include <cmath>
#include <initializer_list>
#include <sstream>

#include "arrow/builder.h"
#include "arrow/compute/api_scalar.h"
#include "arrow/compute/kernels/common.h"
#include "arrow/compute/kernels/temporal_internal.h"
#include "arrow/util/checked_cast.h"
#include "arrow/util/time.h"
#include "arrow/vendored/datetime.h"

namespace arrow {

using internal::checked_cast;
using internal::checked_pointer_cast;

namespace compute {
namespace internal {

namespace {

using arrow_vendored::date::days;
using arrow_vendored::date::floor;
using arrow_vendored::date::hh_mm_ss;
using arrow_vendored::date::local_days;
using arrow_vendored::date::local_time;
using arrow_vendored::date::locate_zone;
using arrow_vendored::date::sys_days;
using arrow_vendored::date::sys_time;
using arrow_vendored::date::time_zone;
using arrow_vendored::date::trunc;
using arrow_vendored::date::weekday;
using arrow_vendored::date::weeks;
using arrow_vendored::date::year_month_day;
using arrow_vendored::date::year_month_weekday;
using arrow_vendored::date::years;
using arrow_vendored::date::zoned_time;
using arrow_vendored::date::literals::dec;
using arrow_vendored::date::literals::jan;
using arrow_vendored::date::literals::last;
using arrow_vendored::date::literals::mon;
using arrow_vendored::date::literals::sun;
using arrow_vendored::date::literals::thu;
using arrow_vendored::date::literals::wed;
using internal::applicator::ScalarBinaryNotNullStatefulEqualTypes;
using internal::applicator::SimpleUnary;

using DayOfWeekState = OptionsWrapper<DayOfWeekOptions>;
using WeekState = OptionsWrapper<WeekOptions>;
using StrftimeState = OptionsWrapper<StrftimeOptions>;
using AssumeTimezoneState = OptionsWrapper<AssumeTimezoneOptions>;

const std::shared_ptr<DataType>& IsoCalendarType() {
  static auto type = struct_({field("iso_year", int64()), field("iso_week", int64()),
                              field("iso_day_of_week", int64())});
  return type;
}

Result<std::locale> GetLocale(const std::string& locale) {
  try {
    return std::locale(locale.c_str());
  } catch (const std::runtime_error& ex) {
    return Status::Invalid("Cannot find locale '", locale, "': ", ex.what());
  }
}

Status CheckTimezones(const ExecBatch& batch) {
  const auto& timezone = GetInputTimezone(batch.values[0]);
  for (int i = 1; i < batch.num_values(); i++) {
    const auto& other_timezone = GetInputTimezone(batch.values[i]);
    if (other_timezone != timezone) {
      return Status::TypeError("Got differing time zone '", other_timezone,
                               "' for argument ", i + 1, "; expected '", timezone, "'");
    }
  }
  return Status::OK();
}

Status ValidateDayOfWeekOptions(const DayOfWeekOptions& options) {
  if (options.week_start < 1 || 7 < options.week_start) {
    return Status::Invalid(
        "week_start must follow ISO convention (Monday=1, Sunday=7). Got week_start=",
        options.week_start);
  }
  return Status::OK();
}

int64_t GetQuarter(const year_month_day& ymd) {
  return static_cast<int64_t>((static_cast<uint32_t>(ymd.month()) - 1) / 3);
}

template <template <typename...> class Op, typename Duration, typename InType,
          typename OutType>
struct TemporalBinary {
  template <typename OptionsType, typename T = InType>
  static enable_if_timestamp<T, Status> ExecWithOptions(KernelContext* ctx,
                                                        const OptionsType* options,
                                                        const ExecBatch& batch,
                                                        Datum* out) {
    RETURN_NOT_OK(CheckTimezones(batch));

    const auto& timezone = GetInputTimezone(batch.values[0]);
    if (timezone.empty()) {
      using ExecTemplate = Op<Duration, NonZonedLocalizer>;
      auto op = ExecTemplate(options, NonZonedLocalizer());
      applicator::ScalarBinaryNotNullStatefulEqualTypes<OutType, T, ExecTemplate> kernel{
          op};
      return kernel.Exec(ctx, batch, out);
    } else {
      ARROW_ASSIGN_OR_RAISE(auto tz, LocateZone(timezone));
      using ExecTemplate = Op<Duration, ZonedLocalizer>;
      auto op = ExecTemplate(options, ZonedLocalizer{tz});
      applicator::ScalarBinaryNotNullStatefulEqualTypes<OutType, T, ExecTemplate> kernel{
          op};
      return kernel.Exec(ctx, batch, out);
    }
  }

  template <typename OptionsType, typename T = InType>
  static enable_if_t<!is_timestamp_type<T>::value, Status> ExecWithOptions(
      KernelContext* ctx, const OptionsType* options, const ExecBatch& batch,
      Datum* out) {
    using ExecTemplate = Op<Duration, NonZonedLocalizer>;
    auto op = ExecTemplate(options, NonZonedLocalizer());
    applicator::ScalarBinaryNotNullStatefulEqualTypes<OutType, T, ExecTemplate> kernel{
        op};
    return kernel.Exec(ctx, batch, out);
  }

  static Status Exec(KernelContext* ctx, const ExecBatch& batch, Datum* out) {
    const FunctionOptions* options = nullptr;
    return ExecWithOptions(ctx, options, batch, out);
  }
};

template <template <typename...> class Op, typename Duration, typename InType,
          typename OutType>
struct TemporalComponentExtractDayOfWeek
    : public TemporalComponentExtractBase<Op, Duration, InType, OutType> {
  using Base = TemporalComponentExtractBase<Op, Duration, InType, OutType>;

  static Status Exec(KernelContext* ctx, const ExecBatch& batch, Datum* out) {
    const DayOfWeekOptions& options = DayOfWeekState::Get(ctx);
    RETURN_NOT_OK(ValidateDayOfWeekOptions(options));
    return Base::ExecWithOptions(ctx, &options, batch, out);
  }
};

template <template <typename...> class Op, typename Duration, typename InType,
          typename OutType>
struct TemporalDayOfWeekBinary : public TemporalBinary<Op, Duration, InType, OutType> {
  using Base = TemporalBinary<Op, Duration, InType, OutType>;

  static Status Exec(KernelContext* ctx, const ExecBatch& batch, Datum* out) {
    const DayOfWeekOptions& options = DayOfWeekState::Get(ctx);
    RETURN_NOT_OK(ValidateDayOfWeekOptions(options));
    return Base::ExecWithOptions(ctx, &options, batch, out);
  }
};

template <template <typename...> class Op, typename Duration, typename InType,
          typename OutType>
struct AssumeTimezoneExtractor
    : public TemporalComponentExtractBase<Op, Duration, InType, OutType> {
  using Base = TemporalComponentExtractBase<Op, Duration, InType, OutType>;

  static Status Exec(KernelContext* ctx, const ExecBatch& batch, Datum* out) {
    const AssumeTimezoneOptions& options = AssumeTimezoneState::Get(ctx);
    const auto& timezone = GetInputTimezone(batch.values[0]);
    if (!timezone.empty()) {
      return Status::Invalid("Timestamps already have a timezone: '", timezone,
                             "'. Cannot localize to '", options.timezone, "'.");
    }
    ARROW_ASSIGN_OR_RAISE(auto tz, LocateZone(options.timezone));
    using ExecTemplate = Op<Duration>;
    auto op = ExecTemplate(&options, tz);
    applicator::ScalarUnaryNotNullStateful<OutType, TimestampType, ExecTemplate> kernel{
        op};
    return kernel.Exec(ctx, batch, out);
  }
};

template <template <typename...> class Op, typename Duration, typename InType,
          typename OutType>
struct TemporalComponentExtractWeek
    : public TemporalComponentExtractBase<Op, Duration, InType, OutType> {
  using Base = TemporalComponentExtractBase<Op, Duration, InType, OutType>;

  static Status Exec(KernelContext* ctx, const ExecBatch& batch, Datum* out) {
    const WeekOptions& options = WeekState::Get(ctx);
    return Base::ExecWithOptions(ctx, &options, batch, out);
  }
};

// ----------------------------------------------------------------------
// Extract year from temporal types
//
// This class and the following (`Month`, etc.) are to be used as the `Op`
// parameter to `TemporalComponentExtract`.

template <typename Duration, typename Localizer>
struct Year {
  Year(const FunctionOptions* options, Localizer&& localizer)
      : localizer_(std::move(localizer)) {}

  template <typename T, typename Arg0>
  T Call(KernelContext*, Arg0 arg, Status*) const {
    return static_cast<T>(static_cast<const int32_t>(
        year_month_day(floor<days>(localizer_.template ConvertTimePoint<Duration>(arg)))
            .year()));
  }

  Localizer localizer_;
};

// ----------------------------------------------------------------------
// Extract month from temporal types

template <typename Duration, typename Localizer>
struct Month {
  explicit Month(const FunctionOptions* options, Localizer&& localizer)
      : localizer_(std::move(localizer)) {}

  template <typename T, typename Arg0>
  T Call(KernelContext*, Arg0 arg, Status*) const {
    return static_cast<T>(static_cast<const uint32_t>(
        year_month_day(floor<days>(localizer_.template ConvertTimePoint<Duration>(arg)))
            .month()));
  }

  Localizer localizer_;
};

// ----------------------------------------------------------------------
// Extract day from temporal types

template <typename Duration, typename Localizer>
struct Day {
  explicit Day(const FunctionOptions* options, Localizer&& localizer)
      : localizer_(std::move(localizer)) {}

  template <typename T, typename Arg0>
  T Call(KernelContext*, Arg0 arg, Status*) const {
    return static_cast<T>(static_cast<const uint32_t>(
        year_month_day(floor<days>(localizer_.template ConvertTimePoint<Duration>(arg)))
            .day()));
  }

  Localizer localizer_;
};

// ----------------------------------------------------------------------
// Extract day of week from temporal types
//
// By default week starts on Monday represented by 0 and ends on Sunday represented
// by 6. Start day of the week (Monday=1, Sunday=7) and numbering start (0 or 1) can be
// set using DayOfWeekOptions

template <typename Duration, typename Localizer>
struct DayOfWeek {
  explicit DayOfWeek(const DayOfWeekOptions* options, Localizer&& localizer)
      : localizer_(std::move(localizer)) {
    for (int i = 0; i < 7; i++) {
      lookup_table[i] = i + 8 - options->week_start;
      lookup_table[i] = (lookup_table[i] > 6) ? lookup_table[i] - 7 : lookup_table[i];
      lookup_table[i] += !options->count_from_zero;
    }
  }

  template <typename T, typename Arg0>
  T Call(KernelContext*, Arg0 arg, Status*) const {
    const auto wd = year_month_weekday(
                        floor<days>(localizer_.template ConvertTimePoint<Duration>(arg)))
                        .weekday()
                        .iso_encoding();
    return lookup_table[wd - 1];
  }

  std::array<int64_t, 7> lookup_table;
  Localizer localizer_;
};

// ----------------------------------------------------------------------
// Extract day of year from temporal types

template <typename Duration, typename Localizer>
struct DayOfYear {
  explicit DayOfYear(const FunctionOptions* options, Localizer&& localizer)
      : localizer_(std::move(localizer)) {}

  template <typename T, typename Arg0>
  T Call(KernelContext*, Arg0 arg, Status*) const {
    const auto t = floor<days>(localizer_.template ConvertTimePoint<Duration>(arg));
    return static_cast<T>(
        (t - localizer_.ConvertDays(year_month_day(t).year() / jan / 0)).count());
  }

  Localizer localizer_;
};

// ----------------------------------------------------------------------
// Extract ISO Year values from temporal types
//
// First week of an ISO year has the majority (4 or more) of it's days in January.
// Last week of an ISO year has the year's last Thursday in it.

template <typename Duration, typename Localizer>
struct ISOYear {
  explicit ISOYear(const FunctionOptions* options, Localizer&& localizer)
      : localizer_(std::move(localizer)) {}

  template <typename T, typename Arg0>
  T Call(KernelContext*, Arg0 arg, Status*) const {
    const auto t = floor<days>(localizer_.template ConvertTimePoint<Duration>(arg));
    auto y = year_month_day{t + days{3}}.year();
    auto start = localizer_.ConvertDays((y - years{1}) / dec / thu[last]) + (mon - thu);
    if (t < start) {
      --y;
    }
    return static_cast<T>(static_cast<int32_t>(y));
  }

  Localizer localizer_;
};

// ----------------------------------------------------------------------
// Extract week from temporal types
//
// First week of an ISO year has the majority (4 or more) of its days in January.
// Last week of an ISO year has the year's last Thursday in it.
// Based on
// https://github.com/HowardHinnant/date/blob/6e921e1b1d21e84a5c82416ba7ecd98e33a436d0/include/date/iso_week.h#L1503

template <typename Duration, typename Localizer>
struct Week {
  explicit Week(const WeekOptions* options, Localizer&& localizer)
      : localizer_(std::move(localizer)),
        count_from_zero_(options->count_from_zero),
        first_week_is_fully_in_year_(options->first_week_is_fully_in_year) {
    if (options->week_starts_monday) {
      if (first_week_is_fully_in_year_) {
        wd_ = mon;
      } else {
        wd_ = thu;
      }
    } else {
      if (first_week_is_fully_in_year_) {
        wd_ = sun;
      } else {
        wd_ = wed;
      }
    }
    if (count_from_zero_) {
      days_offset_ = days{0};
    } else {
      days_offset_ = days{3};
    }
  }

  template <typename T, typename Arg0>
  T Call(KernelContext*, Arg0 arg, Status*) const {
    const auto t = floor<days>(localizer_.template ConvertTimePoint<Duration>(arg));
    auto y = year_month_day{t + days_offset_}.year();

    if (first_week_is_fully_in_year_) {
      auto start = localizer_.ConvertDays(y / jan / wd_[1]);
      if (!count_from_zero_) {
        if (t < start) {
          --y;
          start = localizer_.ConvertDays(y / jan / wd_[1]);
        }
      }
      return static_cast<T>(floor<weeks>(t - start).count() + 1);
    }

    auto start = localizer_.ConvertDays((y - years{1}) / dec / wd_[last]) + (mon - thu);
    if (!count_from_zero_) {
      if (t < start) {
        --y;
        start = localizer_.ConvertDays((y - years{1}) / dec / wd_[last]) + (mon - thu);
      }
    }
    return static_cast<T>(floor<weeks>(t - start).count() + 1);
  }

  Localizer localizer_;
  arrow_vendored::date::weekday wd_;
  arrow_vendored::date::days days_offset_;
  const bool count_from_zero_;
  const bool first_week_is_fully_in_year_;
};

// ----------------------------------------------------------------------
// Extract quarter from temporal types

template <typename Duration, typename Localizer>
struct Quarter {
  explicit Quarter(const FunctionOptions* options, Localizer&& localizer)
      : localizer_(std::move(localizer)) {}

  template <typename T, typename Arg0>
  T Call(KernelContext*, Arg0 arg, Status*) const {
    const auto ymd =
        year_month_day(floor<days>(localizer_.template ConvertTimePoint<Duration>(arg)));
    return static_cast<T>(GetQuarter(ymd) + 1);
  }

  Localizer localizer_;
};

// ----------------------------------------------------------------------
// Extract hour from timestamp

template <typename Duration, typename Localizer>
struct Hour {
  explicit Hour(const FunctionOptions* options, Localizer&& localizer)
      : localizer_(std::move(localizer)) {}

  template <typename T, typename Arg0>
  T Call(KernelContext*, Arg0 arg, Status*) const {
    const auto t = localizer_.template ConvertTimePoint<Duration>(arg);
    return static_cast<T>((t - floor<days>(t)) / std::chrono::hours(1));
  }

  Localizer localizer_;
};

// ----------------------------------------------------------------------
// Extract minute from timestamp

template <typename Duration, typename Localizer>
struct Minute {
  explicit Minute(const FunctionOptions* options, Localizer&& localizer)
      : localizer_(std::move(localizer)) {}

  template <typename T, typename Arg0>
  T Call(KernelContext*, Arg0 arg, Status*) const {
    const auto t = localizer_.template ConvertTimePoint<Duration>(arg);
    return static_cast<T>((t - floor<std::chrono::hours>(t)) / std::chrono::minutes(1));
  }

  Localizer localizer_;
};

// ----------------------------------------------------------------------
// Extract second from timestamp

template <typename Duration, typename Localizer>
struct Second {
  explicit Second(const FunctionOptions* options, Localizer&& localizer) {}

  template <typename T, typename Arg0>
  T Call(KernelContext*, Arg0 arg, Status*) const {
    Duration t = Duration{arg};
    return static_cast<T>((t - floor<std::chrono::minutes>(t)) / std::chrono::seconds(1));
  }
};

// ----------------------------------------------------------------------
// Extract subsecond from timestamp

template <typename Duration, typename Localizer>
struct Subsecond {
  explicit Subsecond(const FunctionOptions* options, Localizer&& localizer) {}

  template <typename T, typename Arg0>
  static T Call(KernelContext*, Arg0 arg, Status*) {
    Duration t = Duration{arg};
    return static_cast<T>(
        (std::chrono::duration<double>(t - floor<std::chrono::seconds>(t)).count()));
  }
};

// ----------------------------------------------------------------------
// Extract milliseconds from timestamp

template <typename Duration, typename Localizer>
struct Millisecond {
  explicit Millisecond(const FunctionOptions* options, Localizer&& localizer) {}

  template <typename T, typename Arg0>
  static T Call(KernelContext*, Arg0 arg, Status*) {
    Duration t = Duration{arg};
    return static_cast<T>(
        ((t - floor<std::chrono::seconds>(t)) / std::chrono::milliseconds(1)) % 1000);
  }
};

// ----------------------------------------------------------------------
// Extract microseconds from timestamp

template <typename Duration, typename Localizer>
struct Microsecond {
  explicit Microsecond(const FunctionOptions* options, Localizer&& localizer) {}

  template <typename T, typename Arg0>
  static T Call(KernelContext*, Arg0 arg, Status*) {
    Duration t = Duration{arg};
    return static_cast<T>(
        ((t - floor<std::chrono::seconds>(t)) / std::chrono::microseconds(1)) % 1000);
  }
};

// ----------------------------------------------------------------------
// Extract nanoseconds from timestamp

template <typename Duration, typename Localizer>
struct Nanosecond {
  explicit Nanosecond(const FunctionOptions* options, Localizer&& localizer) {}

  template <typename T, typename Arg0>
  static T Call(KernelContext*, Arg0 arg, Status*) {
    Duration t = Duration{arg};
    return static_cast<T>(
        ((t - floor<std::chrono::seconds>(t)) / std::chrono::nanoseconds(1)) % 1000);
  }
};

// ----------------------------------------------------------------------
// Convert timestamps to a string representation with an arbitrary format

#ifndef _WIN32
template <typename Duration, typename InType>
struct Strftime {
  const StrftimeOptions& options;
  const time_zone* tz;
  const std::locale locale;

  static Result<Strftime> Make(KernelContext* ctx, const DataType& type) {
    const StrftimeOptions& options = StrftimeState::Get(ctx);

    // This check is due to surprising %c behavior.
    // See https://github.com/HowardHinnant/date/issues/704
    if ((options.format.find("%c") != std::string::npos) && (options.locale != "C")) {
      return Status::Invalid("%c flag is not supported in non-C locales.");
    }
    auto timezone = GetInputTimezone(type);

    if (timezone.empty()) {
      if ((options.format.find("%z") != std::string::npos) ||
          (options.format.find("%Z") != std::string::npos)) {
        return Status::Invalid(
            "Timezone not present, cannot convert to string with timezone: ",
            options.format);
      }
      timezone = "UTC";
    }

    ARROW_ASSIGN_OR_RAISE(const time_zone* tz, LocateZone(timezone));

    ARROW_ASSIGN_OR_RAISE(std::locale locale, GetLocale(options.locale));

    return Strftime{options, tz, std::move(locale)};
  }

  static Status Call(KernelContext* ctx, const Scalar& in, Scalar* out) {
    ARROW_ASSIGN_OR_RAISE(auto self, Make(ctx, *in.type));
    TimestampFormatter formatter{self.options.format, self.tz, self.locale};

    if (in.is_valid) {
      const int64_t in_val = internal::UnboxScalar<const InType>::Unbox(in);
      ARROW_ASSIGN_OR_RAISE(auto formatted, formatter(in_val));
      checked_cast<StringScalar*>(out)->value = Buffer::FromString(std::move(formatted));
    } else {
      out->is_valid = false;
    }
    return Status::OK();
  }

  static Status Call(KernelContext* ctx, const ArrayData& in, ArrayData* out) {
    ARROW_ASSIGN_OR_RAISE(auto self, Make(ctx, *in.type));
    TimestampFormatter formatter{self.options.format, self.tz, self.locale};

    StringBuilder string_builder;
    // Presize string data using a heuristic
    {
      ARROW_ASSIGN_OR_RAISE(auto formatted, formatter(42));
      const auto string_size = static_cast<int64_t>(std::ceil(formatted.size() * 1.1));
      RETURN_NOT_OK(string_builder.Reserve(in.length));
      RETURN_NOT_OK(
          string_builder.ReserveData((in.length - in.GetNullCount()) * string_size));
    }

    auto visit_null = [&]() { return string_builder.AppendNull(); };
    auto visit_value = [&](int64_t arg) {
      ARROW_ASSIGN_OR_RAISE(auto formatted, formatter(arg));
      return string_builder.Append(std::move(formatted));
    };
    RETURN_NOT_OK(VisitArrayDataInline<InType>(in, visit_value, visit_null));

    std::shared_ptr<Array> out_array;
    RETURN_NOT_OK(string_builder.Finish(&out_array));
    *out = *std::move(out_array->data());

    return Status::OK();
  }

  struct TimestampFormatter {
    const char* format;
    const time_zone* tz;
    std::ostringstream bufstream;

    explicit TimestampFormatter(const std::string& format, const time_zone* tz,
                                const std::locale& locale)
        : format(format.c_str()), tz(tz) {
      bufstream.imbue(locale);
      // Propagate errors as C++ exceptions (to get an actual error message)
      bufstream.exceptions(std::ios::failbit | std::ios::badbit);
    }

    Result<std::string> operator()(int64_t arg) {
      bufstream.str("");
      const auto zt = zoned_time<Duration>{tz, sys_time<Duration>(Duration{arg})};
      try {
        arrow_vendored::date::to_stream(bufstream, format, zt);
      } catch (const std::runtime_error& ex) {
        bufstream.clear();
        return Status::Invalid("Failed formatting timestamp: ", ex.what());
      }
      // XXX could return a view with std::ostringstream::view() (C++20)
      return std::move(bufstream).str();
    }
  };
};
#else
template <typename Duration, typename InType>
struct Strftime {
  static Status Call(KernelContext* ctx, const Scalar& in, Scalar* out) {
    return Status::NotImplemented("Strftime not yet implemented on windows.");
  }
  static Status Call(KernelContext* ctx, const ArrayData& in, ArrayData* out) {
    return Status::NotImplemented("Strftime not yet implemented on windows.");
  }
};
#endif

// ----------------------------------------------------------------------
// Convert timestamps from local timestamp without a timezone to timestamps with a
// timezone, interpreting the local timestamp as being in the specified timezone

Result<ValueDescr> ResolveAssumeTimezoneOutput(KernelContext* ctx,
                                               const std::vector<ValueDescr>& args) {
  auto in_type = checked_cast<const TimestampType*>(args[0].type.get());
  auto type = timestamp(in_type->unit(), AssumeTimezoneState::Get(ctx).timezone);
  return ValueDescr(std::move(type));
}

template <typename Duration>
struct AssumeTimezone {
  explicit AssumeTimezone(const AssumeTimezoneOptions* options, const time_zone* tz)
      : options(*options), tz_(tz) {}

  template <typename T, typename Arg0>
  T get_local_time(Arg0 arg, const time_zone* tz) const {
    return static_cast<T>(zoned_time<Duration>(tz, local_time<Duration>(Duration{arg}))
                              .get_sys_time()
                              .time_since_epoch()
                              .count());
  }

  template <typename T, typename Arg0>
  T get_local_time(Arg0 arg, const arrow_vendored::date::choose choose,
                   const time_zone* tz) const {
    return static_cast<T>(
        zoned_time<Duration>(tz, local_time<Duration>(Duration{arg}), choose)
            .get_sys_time()
            .time_since_epoch()
            .count());
  }

  template <typename T, typename Arg0>
  T Call(KernelContext*, Arg0 arg, Status* st) const {
    try {
      return get_local_time<T, Arg0>(arg, tz_);
    } catch (const arrow_vendored::date::nonexistent_local_time& e) {
      switch (options.nonexistent) {
        case AssumeTimezoneOptions::Nonexistent::NONEXISTENT_RAISE: {
          *st = Status::Invalid("Timestamp doesn't exist in timezone '", options.timezone,
                                "': ", e.what());
          return arg;
        }
        case AssumeTimezoneOptions::Nonexistent::NONEXISTENT_EARLIEST: {
          return get_local_time<T, Arg0>(arg, arrow_vendored::date::choose::latest, tz_) -
                 1;
        }
        case AssumeTimezoneOptions::Nonexistent::NONEXISTENT_LATEST: {
          return get_local_time<T, Arg0>(arg, arrow_vendored::date::choose::latest, tz_);
        }
      }
    } catch (const arrow_vendored::date::ambiguous_local_time& e) {
      switch (options.ambiguous) {
        case AssumeTimezoneOptions::Ambiguous::AMBIGUOUS_RAISE: {
          *st = Status::Invalid("Timestamp is ambiguous in timezone '", options.timezone,
                                "': ", e.what());
          return arg;
        }
        case AssumeTimezoneOptions::Ambiguous::AMBIGUOUS_EARLIEST: {
          return get_local_time<T, Arg0>(arg, arrow_vendored::date::choose::earliest,
                                         tz_);
        }
        case AssumeTimezoneOptions::Ambiguous::AMBIGUOUS_LATEST: {
          return get_local_time<T, Arg0>(arg, arrow_vendored::date::choose::latest, tz_);
        }
      }
    }
    return 0;
  }
  AssumeTimezoneOptions options;
  const time_zone* tz_;
};

// ----------------------------------------------------------------------
// Extract ISO calendar values from timestamp

template <typename Duration, typename Localizer>
std::array<int64_t, 3> GetIsoCalendar(int64_t arg, Localizer&& localizer) {
  const auto t = floor<days>(localizer.template ConvertTimePoint<Duration>(arg));
  const auto ymd = year_month_day(t);
  auto y = year_month_day{t + days{3}}.year();
  auto start = localizer.ConvertDays((y - years{1}) / dec / thu[last]) + (mon - thu);
  if (t < start) {
    --y;
    start = localizer.ConvertDays((y - years{1}) / dec / thu[last]) + (mon - thu);
  }
  return {static_cast<int64_t>(static_cast<int32_t>(y)),
          static_cast<int64_t>(trunc<weeks>(t - start).count() + 1),
          static_cast<int64_t>(weekday(ymd).iso_encoding())};
}

template <typename Duration, typename InType>
struct ISOCalendarWrapper {
  static Result<std::array<int64_t, 3>> Get(const Scalar& in) {
    const auto& in_val = internal::UnboxScalar<const InType>::Unbox(in);
    return GetIsoCalendar<Duration>(in_val, NonZonedLocalizer{});
  }
};

template <typename Duration>
struct ISOCalendarWrapper<Duration, TimestampType> {
  static Result<std::array<int64_t, 3>> Get(const Scalar& in) {
    const auto& in_val = internal::UnboxScalar<const TimestampType>::Unbox(in);
    const auto& timezone = GetInputTimezone(in);
    if (timezone.empty()) {
      return GetIsoCalendar<Duration>(in_val, NonZonedLocalizer{});
    } else {
      ARROW_ASSIGN_OR_RAISE(auto tz, LocateZone(timezone));
      return GetIsoCalendar<Duration>(in_val, ZonedLocalizer{tz});
    }
  }
};

template <typename Duration, typename InType, typename BuilderType>
struct ISOCalendarVisitValueFunction {
  static Result<std::function<Status(typename InType::c_type arg)>> Get(
      const std::vector<BuilderType*>& field_builders, const ArrayData&,
      StructBuilder* struct_builder) {
    return [=](typename InType::c_type arg) {
      const auto iso_calendar = GetIsoCalendar<Duration>(arg, NonZonedLocalizer{});
      field_builders[0]->UnsafeAppend(iso_calendar[0]);
      field_builders[1]->UnsafeAppend(iso_calendar[1]);
      field_builders[2]->UnsafeAppend(iso_calendar[2]);
      return struct_builder->Append();
    };
  }
};

template <typename Duration, typename BuilderType>
struct ISOCalendarVisitValueFunction<Duration, TimestampType, BuilderType> {
  static Result<std::function<Status(typename TimestampType::c_type arg)>> Get(
      const std::vector<BuilderType*>& field_builders, const ArrayData& in,
      StructBuilder* struct_builder) {
    const auto& timezone = GetInputTimezone(in);
    if (timezone.empty()) {
      return [=](TimestampType::c_type arg) {
        const auto iso_calendar = GetIsoCalendar<Duration>(arg, NonZonedLocalizer{});
        field_builders[0]->UnsafeAppend(iso_calendar[0]);
        field_builders[1]->UnsafeAppend(iso_calendar[1]);
        field_builders[2]->UnsafeAppend(iso_calendar[2]);
        return struct_builder->Append();
      };
    }
    ARROW_ASSIGN_OR_RAISE(auto tz, LocateZone(timezone));
    return [=](TimestampType::c_type arg) {
      const auto iso_calendar = GetIsoCalendar<Duration>(arg, ZonedLocalizer{tz});
      field_builders[0]->UnsafeAppend(iso_calendar[0]);
      field_builders[1]->UnsafeAppend(iso_calendar[1]);
      field_builders[2]->UnsafeAppend(iso_calendar[2]);
      return struct_builder->Append();
    };
  }
};

template <typename Duration, typename InType>
struct ISOCalendar {
  static Status Call(KernelContext* ctx, const Scalar& in, Scalar* out) {
    if (in.is_valid) {
      ARROW_ASSIGN_OR_RAISE(auto iso_calendar,
                            (ISOCalendarWrapper<Duration, InType>::Get(in)));
      ScalarVector values = {std::make_shared<Int64Scalar>(iso_calendar[0]),
                             std::make_shared<Int64Scalar>(iso_calendar[1]),
                             std::make_shared<Int64Scalar>(iso_calendar[2])};
      *checked_cast<StructScalar*>(out) =
          StructScalar(std::move(values), IsoCalendarType());
    } else {
      out->is_valid = false;
    }
    return Status::OK();
  }

  static Status Call(KernelContext* ctx, const ArrayData& in, ArrayData* out) {
    using BuilderType = typename TypeTraits<Int64Type>::BuilderType;

    std::unique_ptr<ArrayBuilder> array_builder;
    RETURN_NOT_OK(MakeBuilder(ctx->memory_pool(), IsoCalendarType(), &array_builder));
    StructBuilder* struct_builder = checked_cast<StructBuilder*>(array_builder.get());
    RETURN_NOT_OK(struct_builder->Reserve(in.length));

    std::vector<BuilderType*> field_builders;
    field_builders.reserve(3);
    for (int i = 0; i < 3; i++) {
      field_builders.push_back(
          checked_cast<BuilderType*>(struct_builder->field_builder(i)));
      RETURN_NOT_OK(field_builders[i]->Reserve(1));
    }
    auto visit_null = [&]() { return struct_builder->AppendNull(); };
    std::function<Status(typename InType::c_type arg)> visit_value;
    ARROW_ASSIGN_OR_RAISE(
        visit_value, (ISOCalendarVisitValueFunction<Duration, InType, BuilderType>::Get(
                         field_builders, in, struct_builder)));
    RETURN_NOT_OK(
        VisitArrayDataInline<typename InType::PhysicalType>(in, visit_value, visit_null));
    std::shared_ptr<Array> out_array;
    RETURN_NOT_OK(struct_builder->Finish(&out_array));
    *out = *std::move(out_array->data());
    return Status::OK();
  }
};

// ----------------------------------------------------------------------
// Compute boundary crossings between two timestamps

template <typename Duration, typename Localizer>
struct YearsBetween {
  YearsBetween(const FunctionOptions* options, Localizer&& localizer)
      : localizer_(std::move(localizer)) {}

  template <typename T, typename Arg0, typename Arg1>
  T Call(KernelContext*, Arg0 arg0, Arg1 arg1, Status*) const {
    year_month_day from(
        floor<days>(localizer_.template ConvertTimePoint<Duration>(arg0)));
    year_month_day to(floor<days>(localizer_.template ConvertTimePoint<Duration>(arg1)));
    return static_cast<T>((to.year() - from.year()).count());
  }

  Localizer localizer_;
};

template <typename Duration, typename Localizer>
struct QuartersBetween {
  QuartersBetween(const FunctionOptions* options, Localizer&& localizer)
      : localizer_(std::move(localizer)) {}

  static int64_t GetQuarters(const year_month_day& ymd) {
    return static_cast<int64_t>(static_cast<int32_t>(ymd.year())) * 4 + GetQuarter(ymd);
  }

  template <typename T, typename Arg0, typename Arg1>
  T Call(KernelContext*, Arg0 arg0, Arg1 arg1, Status*) const {
    year_month_day from_ymd(
        floor<days>(localizer_.template ConvertTimePoint<Duration>(arg0)));
    year_month_day to_ymd(
        floor<days>(localizer_.template ConvertTimePoint<Duration>(arg1)));
    int64_t from_quarters = GetQuarters(from_ymd);
    int64_t to_quarters = GetQuarters(to_ymd);
    return static_cast<T>(to_quarters - from_quarters);
  }

  Localizer localizer_;
};

template <typename Duration, typename Localizer>
struct MonthsBetween {
  MonthsBetween(const FunctionOptions* options, Localizer&& localizer)
      : localizer_(std::move(localizer)) {}

  template <typename T, typename Arg0, typename Arg1>
  T Call(KernelContext*, Arg0 arg0, Arg1 arg1, Status*) const {
    year_month_day from(
        floor<days>(localizer_.template ConvertTimePoint<Duration>(arg0)));
    year_month_day to(floor<days>(localizer_.template ConvertTimePoint<Duration>(arg1)));
    return static_cast<T>((to.year() / to.month() - from.year() / from.month()).count());
  }

  Localizer localizer_;
};

template <typename Duration, typename Localizer>
struct WeeksBetween {
  using days_t = typename Localizer::days_t;

  WeeksBetween(const DayOfWeekOptions* options, Localizer&& localizer)
      : week_start_(options->week_start), localizer_(std::move(localizer)) {}

  /// Adjust the day backwards to land on the start of the week.
  days_t ToWeekStart(days_t point) const {
    const weekday dow(point);
    const weekday start_of_week(week_start_);
    if (dow == start_of_week) return point;
    const days delta = start_of_week - dow;
    // delta is always positive and in [0, 6]
    return point - days(7 - delta.count());
  }

  template <typename T, typename Arg0, typename Arg1>
  T Call(KernelContext*, Arg0 arg0, Arg1 arg1, Status*) const {
    auto from =
        ToWeekStart(floor<days>(localizer_.template ConvertTimePoint<Duration>(arg0)));
    auto to =
        ToWeekStart(floor<days>(localizer_.template ConvertTimePoint<Duration>(arg1)));
    return (to - from).count() / 7;
  }

  uint32_t week_start_;
  Localizer localizer_;
};

template <typename Duration, typename Localizer>
struct MonthDayNanoBetween {
  MonthDayNanoBetween(const FunctionOptions* options, Localizer&& localizer)
      : localizer_(std::move(localizer)) {}

  template <typename T, typename Arg0, typename Arg1>
  T Call(KernelContext*, Arg0 arg0, Arg1 arg1, Status*) const {
    static_assert(std::is_same<T, MonthDayNanoIntervalType::MonthDayNanos>::value, "");
    auto from = localizer_.template ConvertTimePoint<Duration>(arg0);
    auto to = localizer_.template ConvertTimePoint<Duration>(arg1);
    year_month_day from_ymd(floor<days>(from));
    year_month_day to_ymd(floor<days>(to));
    const int32_t num_months = static_cast<int32_t>(
        (to_ymd.year() / to_ymd.month() - from_ymd.year() / from_ymd.month()).count());
    const int32_t num_days = static_cast<int32_t>(static_cast<uint32_t>(to_ymd.day())) -
                             static_cast<int32_t>(static_cast<uint32_t>(from_ymd.day()));
    auto from_time = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(from - floor<days>(from))
            .count());
    auto to_time = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(to - floor<days>(to))
            .count());
    const int64_t num_nanos = to_time - from_time;
    return T{num_months, num_days, num_nanos};
  }

  Localizer localizer_;
};

template <typename Duration, typename Localizer>
struct DayTimeBetween {
  DayTimeBetween(const FunctionOptions* options, Localizer&& localizer)
      : localizer_(std::move(localizer)) {}

  template <typename T, typename Arg0, typename Arg1>
  T Call(KernelContext*, Arg0 arg0, Arg1 arg1, Status*) const {
    static_assert(std::is_same<T, DayTimeIntervalType::DayMilliseconds>::value, "");
    auto from = localizer_.template ConvertTimePoint<Duration>(arg0);
    auto to = localizer_.template ConvertTimePoint<Duration>(arg1);
    const int32_t num_days =
        static_cast<int32_t>((floor<days>(to) - floor<days>(from)).count());
    auto from_time = static_cast<int32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(from - floor<days>(from))
            .count());
    auto to_time = static_cast<int32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(to - floor<days>(to))
            .count());
    const int32_t num_millis = to_time - from_time;
    return DayTimeIntervalType::DayMilliseconds{num_days, num_millis};
  }

  Localizer localizer_;
};

template <typename Unit, typename Duration, typename Localizer>
struct UnitsBetween {
  UnitsBetween(const FunctionOptions* options, Localizer&& localizer)
      : localizer_(std::move(localizer)) {}

  template <typename T, typename Arg0, typename Arg1>
  T Call(KernelContext*, Arg0 arg0, Arg1 arg1, Status*) const {
    auto from = floor<Unit>(localizer_.template ConvertTimePoint<Duration>(arg0));
    auto to = floor<Unit>(localizer_.template ConvertTimePoint<Duration>(arg1));
    return static_cast<T>((to - from).count());
  }

  Localizer localizer_;
};

template <typename Duration, typename Localizer>
using DaysBetween = UnitsBetween<days, Duration, Localizer>;

template <typename Duration, typename Localizer>
using HoursBetween = UnitsBetween<std::chrono::hours, Duration, Localizer>;

template <typename Duration, typename Localizer>
using MinutesBetween = UnitsBetween<std::chrono::minutes, Duration, Localizer>;

template <typename Duration, typename Localizer>
using SecondsBetween = UnitsBetween<std::chrono::seconds, Duration, Localizer>;

template <typename Duration, typename Localizer>
using MillisecondsBetween = UnitsBetween<std::chrono::milliseconds, Duration, Localizer>;

template <typename Duration, typename Localizer>
using MicrosecondsBetween = UnitsBetween<std::chrono::microseconds, Duration, Localizer>;

template <typename Duration, typename Localizer>
using NanosecondsBetween = UnitsBetween<std::chrono::nanoseconds, Duration, Localizer>;

// ----------------------------------------------------------------------
// Registration helpers

// Which types to generate a kernel for
enum EnabledTypes : uint8_t { WithDates, WithTimes, WithTimestamps };

template <template <typename...> class Op,
          template <template <typename...> class OpExec, typename Duration,
                    typename InType, typename OutType, typename... Args>
          class ExecTemplate,
          typename OutType>
std::shared_ptr<ScalarFunction> MakeTemporal(
    std::string name, std::initializer_list<EnabledTypes> in_types, OutputType out_type,
    const FunctionDoc* doc, const FunctionOptions* default_options = NULLPTR,
    KernelInit init = NULLPTR) {
  DCHECK_NE(in_types.size(), 0);
  auto func =
      std::make_shared<ScalarFunction>(name, Arity::Unary(), doc, default_options);

  for (const auto in_type : in_types) {
    switch (in_type) {
      case WithDates: {
        auto exec32 = ExecTemplate<Op, days, Date32Type, OutType>::Exec;
        DCHECK_OK(func->AddKernel({date32()}, out_type, std::move(exec32), init));
        auto exec64 =
            ExecTemplate<Op, std::chrono::milliseconds, Date64Type, OutType>::Exec;
        DCHECK_OK(func->AddKernel({date64()}, out_type, std::move(exec64), init));
        break;
      }

      case WithTimes: {
        auto exec32s = ExecTemplate<Op, std::chrono::seconds, Time32Type, OutType>::Exec;
        DCHECK_OK(func->AddKernel({time32(TimeUnit::SECOND)}, out_type,
                                  std::move(exec32s), init));
        auto exec32ms =
            ExecTemplate<Op, std::chrono::milliseconds, Time32Type, OutType>::Exec;
        DCHECK_OK(func->AddKernel({time32(TimeUnit::MILLI)}, out_type,
                                  std::move(exec32ms), init));
        auto exec64us =
            ExecTemplate<Op, std::chrono::microseconds, Time64Type, OutType>::Exec;
        DCHECK_OK(func->AddKernel({time64(TimeUnit::MICRO)}, out_type,
                                  std::move(exec64us), init));
        auto exec64ns =
            ExecTemplate<Op, std::chrono::nanoseconds, Time64Type, OutType>::Exec;
        DCHECK_OK(func->AddKernel({time64(TimeUnit::NANO)}, out_type, std::move(exec64ns),
                                  init));
        break;
      }

      case WithTimestamps: {
        for (auto unit : TimeUnit::values()) {
          InputType in_type{match::TimestampTypeUnit(unit)};
          switch (unit) {
            case TimeUnit::SECOND: {
              auto exec =
                  ExecTemplate<Op, std::chrono::seconds, TimestampType, OutType>::Exec;
              DCHECK_OK(func->AddKernel({in_type}, out_type, std::move(exec), init));
              break;
            }
            case TimeUnit::MILLI: {
              auto exec = ExecTemplate<Op, std::chrono::milliseconds, TimestampType,
                                       OutType>::Exec;
              DCHECK_OK(func->AddKernel({in_type}, out_type, std::move(exec), init));
              break;
            }
            case TimeUnit::MICRO: {
              auto exec = ExecTemplate<Op, std::chrono::microseconds, TimestampType,
                                       OutType>::Exec;
              DCHECK_OK(func->AddKernel({in_type}, out_type, std::move(exec), init));
              break;
            }
            case TimeUnit::NANO: {
              auto exec = ExecTemplate<Op, std::chrono::nanoseconds, TimestampType,
                                       OutType>::Exec;
              DCHECK_OK(func->AddKernel({in_type}, out_type, std::move(exec), init));
              break;
            }
          }
        }
        break;
      }
    }
  }

  return func;
}

template <template <typename...> class Op>
std::shared_ptr<ScalarFunction> MakeSimpleUnaryTemporal(
    std::string name, std::initializer_list<EnabledTypes> in_types,
    const std::shared_ptr<arrow::DataType> out_type, const FunctionDoc* doc,
    const FunctionOptions* default_options = NULLPTR, KernelInit init = NULLPTR) {
  DCHECK_NE(in_types.size(), 0);
  auto func =
      std::make_shared<ScalarFunction>(name, Arity::Unary(), doc, default_options);

  for (const auto in_type : in_types) {
    switch (in_type) {
      case WithDates: {
        auto exec32 = SimpleUnary<Op<days, Date32Type>>;
        DCHECK_OK(func->AddKernel({date32()}, out_type, std::move(exec32), init));
        auto exec64 = SimpleUnary<Op<std::chrono::milliseconds, Date64Type>>;
        DCHECK_OK(func->AddKernel({date64()}, out_type, std::move(exec64), init));
        break;
      }

      case WithTimes: {
        auto exec32s = SimpleUnary<Op<std::chrono::seconds, Time32Type>>;
        DCHECK_OK(func->AddKernel({time32(TimeUnit::SECOND)}, out_type,
                                  std::move(exec32s), init));
        auto exec32ms = SimpleUnary<Op<std::chrono::milliseconds, Time32Type>>;
        DCHECK_OK(func->AddKernel({time32(TimeUnit::MILLI)}, out_type,
                                  std::move(exec32ms), init));
        auto exec64us = SimpleUnary<Op<std::chrono::microseconds, Time64Type>>;
        DCHECK_OK(func->AddKernel({time64(TimeUnit::MICRO)}, out_type,
                                  std::move(exec64us), init));
        auto exec64ns = SimpleUnary<Op<std::chrono::nanoseconds, Time64Type>>;
        DCHECK_OK(func->AddKernel({time64(TimeUnit::NANO)}, out_type, std::move(exec64ns),
                                  init));
        break;
      }

      case WithTimestamps: {
        for (auto unit : TimeUnit::values()) {
          InputType in_type{match::TimestampTypeUnit(unit)};
          switch (unit) {
            case TimeUnit::SECOND: {
              auto exec = SimpleUnary<Op<std::chrono::seconds, TimestampType>>;
              DCHECK_OK(func->AddKernel({in_type}, out_type, std::move(exec), init));
              break;
            }
            case TimeUnit::MILLI: {
              auto exec = SimpleUnary<Op<std::chrono::milliseconds, TimestampType>>;
              DCHECK_OK(func->AddKernel({in_type}, out_type, std::move(exec), init));
              break;
            }
            case TimeUnit::MICRO: {
              auto exec = SimpleUnary<Op<std::chrono::microseconds, TimestampType>>;
              DCHECK_OK(func->AddKernel({in_type}, out_type, std::move(exec), init));
              break;
            }
            case TimeUnit::NANO: {
              auto exec = SimpleUnary<Op<std::chrono::nanoseconds, TimestampType>>;
              DCHECK_OK(func->AddKernel({in_type}, out_type, std::move(exec), init));
              break;
            }
          }
        }
        break;
      }
    }
  }

  return func;
}

template <template <typename...> class Op,
          template <template <typename...> class OpExec, typename Duration,
                    typename InType, typename OutType>
          class ExecTemplate,
          typename OutType>
std::shared_ptr<ScalarFunction> MakeTemporalBinary(
    std::string name, std::initializer_list<EnabledTypes> in_types,
    const std::shared_ptr<arrow::DataType> out_type, const FunctionDoc* doc,
    const FunctionOptions* default_options = NULLPTR, KernelInit init = NULLPTR) {
  DCHECK_GT(in_types.size(), 0);
  auto func =
      std::make_shared<ScalarFunction>(name, Arity::Binary(), doc, default_options);

  for (const auto in_type : in_types) {
    switch (in_type) {
      case WithDates: {
        auto exec32 = ExecTemplate<Op, days, Date32Type, OutType>::Exec;
        DCHECK_OK(
            func->AddKernel({date32(), date32()}, out_type, std::move(exec32), init));
        auto exec64 =
            ExecTemplate<Op, std::chrono::milliseconds, Date64Type, OutType>::Exec;
        DCHECK_OK(
            func->AddKernel({date64(), date64()}, out_type, std::move(exec64), init));
        break;
      }
      case WithTimes: {
        auto exec32s = ExecTemplate<Op, std::chrono::seconds, Time32Type, OutType>::Exec;
        auto ty = time32(TimeUnit::SECOND);
        DCHECK_OK(func->AddKernel({ty, ty}, out_type, std::move(exec32s), init));
        auto exec32ms =
            ExecTemplate<Op, std::chrono::milliseconds, Time32Type, OutType>::Exec;
        ty = time32(TimeUnit::MILLI);
        DCHECK_OK(func->AddKernel({ty, ty}, out_type, std::move(exec32ms), init));
        auto exec64us =
            ExecTemplate<Op, std::chrono::microseconds, Time64Type, OutType>::Exec;
        ty = time64(TimeUnit::MICRO);
        DCHECK_OK(func->AddKernel({ty, ty}, out_type, std::move(exec64us), init));
        auto exec64ns =
            ExecTemplate<Op, std::chrono::nanoseconds, Time64Type, OutType>::Exec;
        ty = time64(TimeUnit::NANO);
        DCHECK_OK(func->AddKernel({ty, ty}, out_type, std::move(exec64ns), init));
        break;
      }
      case WithTimestamps: {
        for (auto unit : TimeUnit::values()) {
          InputType in_type{match::TimestampTypeUnit(unit)};
          switch (unit) {
            case TimeUnit::SECOND: {
              auto exec =
                  ExecTemplate<Op, std::chrono::seconds, TimestampType, OutType>::Exec;
              DCHECK_OK(
                  func->AddKernel({in_type, in_type}, out_type, std::move(exec), init));
              break;
            }
            case TimeUnit::MILLI: {
              auto exec = ExecTemplate<Op, std::chrono::milliseconds, TimestampType,
                                       OutType>::Exec;
              DCHECK_OK(
                  func->AddKernel({in_type, in_type}, out_type, std::move(exec), init));
              break;
            }
            case TimeUnit::MICRO: {
              auto exec = ExecTemplate<Op, std::chrono::microseconds, TimestampType,
                                       OutType>::Exec;
              DCHECK_OK(
                  func->AddKernel({in_type, in_type}, out_type, std::move(exec), init));
              break;
            }
            case TimeUnit::NANO: {
              auto exec = ExecTemplate<Op, std::chrono::nanoseconds, TimestampType,
                                       OutType>::Exec;
              DCHECK_OK(
                  func->AddKernel({in_type, in_type}, out_type, std::move(exec), init));
              break;
            }
          }
        }
        break;
      }
    }
  }
  return func;
}

const FunctionDoc year_doc{
    "Extract year number",
    ("Null values emit null.\n"
     "An error is returned if the values have a defined timezone but it\n"
     "cannot be found in the timezone database."),
    {"values"}};

const FunctionDoc month_doc{
    "Extract month number",
    ("Month is encoded as January=1, December=12.\n"
     "Null values emit null.\n"
     "An error is returned if the values have a defined timezone but it\n"
     "cannot be found in the timezone database."),
    {"values"}};

const FunctionDoc day_doc{
    "Extract day number",
    ("Null values emit null.\n"
     "An error is returned if the values have a defined timezone but it\n"
     "cannot be found in the timezone database."),
    {"values"}};

const FunctionDoc day_of_week_doc{
    "Extract day of the week number",
    ("By default, the week starts on Monday represented by 0 and ends on Sunday\n"
     "represented by 6.\n"
     "`DayOfWeekOptions.week_start` can be used to set another starting day using\n"
     "the ISO numbering convention (1=start week on Monday, 7=start week on Sunday).\n"
     "Day numbers can start at 0 or 1 based on `DayOfWeekOptions.count_from_zero`.\n"
     "Null values emit null.\n"
     "An error is returned if the values have a defined timezone but it\n"
     "cannot be found in the timezone database."),
    {"values"},
    "DayOfWeekOptions"};

const FunctionDoc day_of_year_doc{
    "Extract day of year number",
    ("January 1st maps to day number 1, February 1st to 32, etc.\n"
     "Null values emit null.\n"
     "An error is returned if the values have a defined timezone but it\n"
     "cannot be found in the timezone database."),
    {"values"}};

const FunctionDoc iso_year_doc{
    "Extract ISO year number",
    ("First week of an ISO year has the majority (4 or more) of its days in January."
     "Null values emit null.\n"
     "An error is returned if the values have a defined timezone but it\n"
     "cannot be found in the timezone database."),
    {"values"}};

const FunctionDoc iso_week_doc{
    "Extract ISO week of year number",
    ("First ISO week has the majority (4 or more) of its days in January."
     "ISO week starts on Monday.\n"
     "Week of the year starts with 1 and can run up to 53.\n"
     "Null values emit null.\n"
     "An error is returned if the values have a defined timezone but it\n"
     "cannot be found in the timezone database."),
    {"values"}};

const FunctionDoc us_week_doc{
    "Extract US week of year number",
    ("First US week has the majority (4 or more) of its days in January."
     "US week starts on Sunday.\n"
     "Week of the year starts with 1 and can run up to 53.\n"
     "Null values emit null.\n"
     "An error is returned if the timestamps have a defined timezone but it\n"
     "cannot be found in the timezone database."),
    {"values"}};

const FunctionDoc week_doc{
    "Extract week of year number",
    ("First week has the majority (4 or more) of its days in January.\n"
     "Year can have 52 or 53 weeks. Week numbering can start with 0 or 1 using "
     "DayOfWeekOptions.count_from_zero.\n"
     "An error is returned if the timestamps have a defined timezone but it\n"
     "cannot be found in the timezone database."),
    {"values"},
    "WeekOptions"};

const FunctionDoc iso_calendar_doc{
    "Extract (ISO year, ISO week, ISO day of week) struct",
    ("ISO week starts on Monday denoted by 1 and ends on Sunday denoted by 7.\n"
     "Null values emit null.\n"
     "An error is returned if the values have a defined timezone but it\n"
     "cannot be found in the timezone database."),
    {"values"}};

const FunctionDoc quarter_doc{
    "Extract quarter of year number",
    ("First quarter maps to 1 and forth quarter maps to 4.\n"
     "Null values emit null.\n"
     "An error is returned if the values have a defined timezone but it\n"
     "cannot be found in the timezone database."),
    {"values"}};

const FunctionDoc hour_doc{
    "Extract hour value",
    ("Null values emit null.\n"
     "An error is returned if the values have a defined timezone but it\n"
     "cannot be found in the timezone database."),
    {"values"}};

const FunctionDoc minute_doc{
    "Extract minute values",
    ("Null values emit null.\n"
     "An error is returned if the values have a defined timezone but it\n"
     "cannot be found in the timezone database."),
    {"values"}};

const FunctionDoc second_doc{
    "Extract second values",
    ("Null values emit null.\n"
     "An error is returned if the values have a defined timezone but it\n"
     "cannot be found in the timezone database."),
    {"values"}};

const FunctionDoc millisecond_doc{
    "Extract millisecond values",
    ("Millisecond returns number of milliseconds since the last full second.\n"
     "Null values emit null.\n"
     "An error is returned if the values have a defined timezone but it\n"
     "cannot be found in the timezone database."),
    {"values"}};

const FunctionDoc microsecond_doc{
    "Extract microsecond values",
    ("Millisecond returns number of microseconds since the last full millisecond.\n"
     "Null values emit null.\n"
     "An error is returned if the values have a defined timezone but it\n"
     "cannot be found in the timezone database."),
    {"values"}};

const FunctionDoc nanosecond_doc{
    "Extract nanosecond values",
    ("Nanosecond returns number of nanoseconds since the last full microsecond.\n"
     "Null values emit null.\n"
     "An error is returned if the values have a defined timezone but it\n"
     "cannot be found in the timezone database."),
    {"values"}};

const FunctionDoc subsecond_doc{
    "Extract subsecond values",
    ("Subsecond returns the fraction of a second since the last full second.\n"
     "Null values emit null.\n"
     "An error is returned if the values have a defined timezone but it\n"
     "cannot be found in the timezone database."),
    {"values"}};

const FunctionDoc strftime_doc{
    "Format temporal values according to a format string",
    ("For each input value, emit a formatted string.\n"
     "The time format string and locale can be set using StrftimeOptions.\n"
     "The output precision of the \"%S\" (seconds) format code depends on\n"
     "the input time precision: it is an integer for timestamps with\n"
     "second precision, a real number with the required number of fractional\n"
     "digits for higher precisions.\n"
     "Null values emit null.\n"
     "An error is returned if the values have a defined timezone but it\n"
     "cannot be found in the timezone database, or if the specified locale\n"
     "does not exist on this system."),
    {"timestamps"},
    "StrftimeOptions"};

const FunctionDoc assume_timezone_doc{
    "Convert naive timestamp to timezone-aware timestamp",
    ("Input timestamps are assumed to be relative to the timezone given in the\n"
     "`timezone` option. They are converted to UTC-relative timestamps and\n"
     "the output type has its timezone set to the value of the `timezone`\n"
     "option. Null values emit null.\n"
     "This function is meant to be used when an external system produces\n"
     "\"timezone-naive\" timestamps which need to be converted to\n"
     "\"timezone-aware\" timestamps. An error is returned if the timestamps\n"
     "already have a defined timezone."),
    {"timestamps"},
    "AssumeTimezoneOptions"};

const FunctionDoc years_between_doc{
    "Compute the number of years between two timestamps",
    ("Returns the number of year boundaries crossed from `start` to `end`.\n"
     "That is, the difference is calculated as if the timestamps were\n"
     "truncated to the year.\n"
     "Null values emit null."),
    {"start", "end"}};

const FunctionDoc quarters_between_doc{
    "Compute the number of quarters between two timestamps",
    ("Returns the number of quarter start boundaries crossed from `start` to `end`.\n"
     "That is, the difference is calculated as if the timestamps were\n"
     "truncated to the quarter.\n"
     "Null values emit null."),
    {"start", "end"}};

const FunctionDoc months_between_doc{
    "Compute the number of months between two timestamps",
    ("Returns the number of month boundaries crossed from `start` to `end`.\n"
     "That is, the difference is calculated as if the timestamps were\n"
     "truncated to the month.\n"
     "Null values emit null."),
    {"start", "end"}};

const FunctionDoc month_day_nano_interval_between_doc{
    "Compute the number of months, days and nanoseconds between two timestamps",
    ("Returns the number of months, days, and nanoseconds from `start` to `end`.\n"
     "That is, first the difference in months is computed as if both timestamps\n"
     "were truncated to the months, then the difference between the days\n"
     "is computed, and finally the difference between the times of the two\n"
     "timestamps is computed as if both times were truncated to the nanosecond.\n"
     "Null values return null."),
    {"start", "end"}};

const FunctionDoc weeks_between_doc{
    "Compute the number of weeks between two timestamps",
    ("Returns the number of week boundaries crossed from `start` to `end`.\n"
     "That is, the difference is calculated as if the timestamps were\n"
     "truncated to the week.\n"
     "Null values emit null."),
    {"start", "end"},
    "DayOfWeekOptions"};

const FunctionDoc day_time_interval_between_doc{
    "Compute the number of days and milliseconds between two timestamps",
    ("Returns the number of days and milliseconds from `start` to `end`.\n"
     "That is, first the difference in days is computed as if both\n"
     "timestamps were truncated to the day, then the difference between time times\n"
     "of the two timestamps is computed as if both times were truncated to the\n"
     "millisecond.\n"
     "Null values return null."),
    {"start", "end"}};

const FunctionDoc days_between_doc{
    "Compute the number of days between two timestamps",
    ("Returns the number of day boundaries crossed from `start` to `end`.\n"
     "That is, the difference is calculated as if the timestamps were\n"
     "truncated to the day.\n"
     "Null values emit null."),
    {"start", "end"}};

const FunctionDoc hours_between_doc{
    "Compute the number of hours between two timestamps",
    ("Returns the number of hour boundaries crossed from `start` to `end`.\n"
     "That is, the difference is calculated as if the timestamps were\n"
     "truncated to the hour.\n"
     "Null values emit null."),
    {"start", "end"}};

const FunctionDoc minutes_between_doc{
    "Compute the number of minute boundaries between two timestamps",
    ("Returns the number of minute boundaries crossed from `start` to `end`.\n"
     "That is, the difference is calculated as if the timestamps were\n"
     "truncated to the minute.\n"
     "Null values emit null."),
    {"start", "end"}};

const FunctionDoc seconds_between_doc{
    "Compute the number of seconds between two timestamps",
    ("Returns the number of second boundaries crossed from `start` to `end`.\n"
     "That is, the difference is calculated as if the timestamps were\n"
     "truncated to the second.\n"
     "Null values emit null."),
    {"start", "end"}};

const FunctionDoc milliseconds_between_doc{
    "Compute the number of millisecond boundaries between two timestamps",
    ("Returns the number of millisecond boundaries crossed from `start` to `end`.\n"
     "That is, the difference is calculated as if the timestamps were\n"
     "truncated to the millisecond.\n"
     "Null values emit null."),
    {"start", "end"}};

const FunctionDoc microseconds_between_doc{
    "Compute the number of microseconds between two timestamps",
    ("Returns the number of microsecond boundaries crossed from `start` to `end`.\n"
     "That is, the difference is calculated as if the timestamps were\n"
     "truncated to the microsecond.\n"
     "Null values emit null."),
    {"start", "end"}};

const FunctionDoc nanoseconds_between_doc{
    "Compute the number of nanoseconds between two timestamps",
    ("Returns the number of nanosecond boundaries crossed from `start` to `end`.\n"
     "That is, the difference is calculated as if the timestamps were\n"
     "truncated to the nanosecond.\n"
     "Null values emit null."),
    {"start", "end"}};
}  // namespace

void RegisterScalarTemporal(FunctionRegistry* registry) {
  // Date extractors

  auto year = MakeTemporal<Year, TemporalComponentExtract, Int64Type>(
      "year", {WithDates, WithTimestamps}, int64(), &year_doc);
  DCHECK_OK(registry->AddFunction(std::move(year)));

  auto month = MakeTemporal<Month, TemporalComponentExtract, Int64Type>(
      "month", {WithDates, WithTimestamps}, int64(), &month_doc);
  DCHECK_OK(registry->AddFunction(std::move(month)));

  auto day = MakeTemporal<Day, TemporalComponentExtract, Int64Type>(
      "day", {WithDates, WithTimestamps}, int64(), &day_doc);
  DCHECK_OK(registry->AddFunction(std::move(day)));

  static const auto default_day_of_week_options = DayOfWeekOptions::Defaults();
  auto day_of_week =
      MakeTemporal<DayOfWeek, TemporalComponentExtractDayOfWeek, Int64Type>(
          "day_of_week", {WithDates, WithTimestamps}, int64(), &day_of_week_doc,
          &default_day_of_week_options, DayOfWeekState::Init);
  DCHECK_OK(registry->AddFunction(std::move(day_of_week)));

  auto day_of_year = MakeTemporal<DayOfYear, TemporalComponentExtract, Int64Type>(
      "day_of_year", {WithDates, WithTimestamps}, int64(), &day_of_year_doc);
  DCHECK_OK(registry->AddFunction(std::move(day_of_year)));

  auto iso_year = MakeTemporal<ISOYear, TemporalComponentExtract, Int64Type>(
      "iso_year", {WithDates, WithTimestamps}, int64(), &iso_year_doc);
  DCHECK_OK(registry->AddFunction(std::move(iso_year)));

  static const auto default_iso_week_options = WeekOptions::ISODefaults();
  auto iso_week = MakeTemporal<Week, TemporalComponentExtractWeek, Int64Type>(
      "iso_week", {WithDates, WithTimestamps}, int64(), &iso_week_doc,
      &default_iso_week_options, WeekState::Init);
  DCHECK_OK(registry->AddFunction(std::move(iso_week)));

  static const auto default_us_week_options = WeekOptions::USDefaults();
  auto us_week = MakeTemporal<Week, TemporalComponentExtractWeek, Int64Type>(
      "us_week", {WithDates, WithTimestamps}, int64(), &us_week_doc,
      &default_us_week_options, WeekState::Init);
  DCHECK_OK(registry->AddFunction(std::move(us_week)));

  static const auto default_week_options = WeekOptions();
  auto week = MakeTemporal<Week, TemporalComponentExtractWeek, Int64Type>(
      "week", {WithDates, WithTimestamps}, int64(), &week_doc, &default_week_options,
      WeekState::Init);
  DCHECK_OK(registry->AddFunction(std::move(week)));

  auto iso_calendar = MakeSimpleUnaryTemporal<ISOCalendar>(
      "iso_calendar", {WithDates, WithTimestamps}, IsoCalendarType(), &iso_calendar_doc);
  DCHECK_OK(registry->AddFunction(std::move(iso_calendar)));

  auto quarter = MakeTemporal<Quarter, TemporalComponentExtract, Int64Type>(
      "quarter", {WithDates, WithTimestamps}, int64(), &quarter_doc);
  DCHECK_OK(registry->AddFunction(std::move(quarter)));

  // Date / time extractors

  auto hour = MakeTemporal<Hour, TemporalComponentExtract, Int64Type>(
      "hour", {WithTimes, WithTimestamps}, int64(), &hour_doc);
  DCHECK_OK(registry->AddFunction(std::move(hour)));

  auto minute = MakeTemporal<Minute, TemporalComponentExtract, Int64Type>(
      "minute", {WithTimes, WithTimestamps}, int64(), &minute_doc);
  DCHECK_OK(registry->AddFunction(std::move(minute)));

  auto second = MakeTemporal<Second, TemporalComponentExtract, Int64Type>(
      "second", {WithTimes, WithTimestamps}, int64(), &second_doc);
  DCHECK_OK(registry->AddFunction(std::move(second)));

  auto millisecond = MakeTemporal<Millisecond, TemporalComponentExtract, Int64Type>(
      "millisecond", {WithTimes, WithTimestamps}, int64(), &millisecond_doc);
  DCHECK_OK(registry->AddFunction(std::move(millisecond)));

  auto microsecond = MakeTemporal<Microsecond, TemporalComponentExtract, Int64Type>(
      "microsecond", {WithTimes, WithTimestamps}, int64(), &microsecond_doc);
  DCHECK_OK(registry->AddFunction(std::move(microsecond)));

  auto nanosecond = MakeTemporal<Nanosecond, TemporalComponentExtract, Int64Type>(
      "nanosecond", {WithTimes, WithTimestamps}, int64(), &nanosecond_doc);
  DCHECK_OK(registry->AddFunction(std::move(nanosecond)));

  auto subsecond = MakeTemporal<Subsecond, TemporalComponentExtract, DoubleType>(
      "subsecond", {WithTimes, WithTimestamps}, float64(), &subsecond_doc);
  DCHECK_OK(registry->AddFunction(std::move(subsecond)));

  // Timezone-related functions

  static const auto default_strftime_options = StrftimeOptions();
  auto strftime = MakeSimpleUnaryTemporal<Strftime>(
      "strftime", {WithTimes, WithDates, WithTimestamps}, utf8(), &strftime_doc,
      &default_strftime_options, StrftimeState::Init);
  DCHECK_OK(registry->AddFunction(std::move(strftime)));

  auto assume_timezone =
      MakeTemporal<AssumeTimezone, AssumeTimezoneExtractor, TimestampType>(
          "assume_timezone", {WithTimestamps},
          OutputType::Resolver(ResolveAssumeTimezoneOutput), &assume_timezone_doc,
          nullptr, AssumeTimezoneState::Init);
  DCHECK_OK(registry->AddFunction(std::move(assume_timezone)));

  auto years_between = MakeTemporalBinary<YearsBetween, TemporalBinary, Int64Type>(
      "years_between", {WithDates, WithTimestamps}, int64(), &years_between_doc);
  DCHECK_OK(registry->AddFunction(std::move(years_between)));

  auto quarters_between = MakeTemporalBinary<QuartersBetween, TemporalBinary, Int64Type>(
      "quarters_between", {WithDates, WithTimestamps}, int64(), &quarters_between_doc);
  DCHECK_OK(registry->AddFunction(std::move(quarters_between)));

  auto month_interval_between =
      MakeTemporalBinary<MonthsBetween, TemporalBinary, MonthIntervalType>(
          "month_interval_between", {WithDates, WithTimestamps}, month_interval(),
          &months_between_doc);
  DCHECK_OK(registry->AddFunction(std::move(month_interval_between)));

  auto month_day_nano_interval_between =
      MakeTemporalBinary<MonthDayNanoBetween, TemporalBinary, MonthDayNanoIntervalType>(
          "month_day_nano_interval_between", {WithDates, WithTimes, WithTimestamps},
          month_day_nano_interval(), &month_day_nano_interval_between_doc);
  DCHECK_OK(registry->AddFunction(std::move(month_day_nano_interval_between)));

  auto weeks_between =
      MakeTemporalBinary<WeeksBetween, TemporalDayOfWeekBinary, Int64Type>(
          "weeks_between", {WithDates, WithTimestamps}, int64(), &weeks_between_doc,
          &default_day_of_week_options, DayOfWeekState::Init);
  DCHECK_OK(registry->AddFunction(std::move(weeks_between)));

  auto day_time_interval_between =
      MakeTemporalBinary<DayTimeBetween, TemporalBinary, DayTimeIntervalType>(
          "day_time_interval_between", {WithDates, WithTimes, WithTimestamps},
          day_time_interval(), &day_time_interval_between_doc);
  DCHECK_OK(registry->AddFunction(std::move(day_time_interval_between)));

  auto days_between = MakeTemporalBinary<DaysBetween, TemporalBinary, Int64Type>(
      "days_between", {WithDates, WithTimestamps}, int64(), &days_between_doc);
  DCHECK_OK(registry->AddFunction(std::move(days_between)));

  auto hours_between = MakeTemporalBinary<HoursBetween, TemporalBinary, Int64Type>(
      "hours_between", {WithDates, WithTimes, WithTimestamps}, int64(),
      &hours_between_doc);
  DCHECK_OK(registry->AddFunction(std::move(hours_between)));

  auto minutes_between = MakeTemporalBinary<MinutesBetween, TemporalBinary, Int64Type>(
      "minutes_between", {WithDates, WithTimes, WithTimestamps}, int64(),
      &minutes_between_doc);
  DCHECK_OK(registry->AddFunction(std::move(minutes_between)));

  auto seconds_between = MakeTemporalBinary<SecondsBetween, TemporalBinary, Int64Type>(
      "seconds_between", {WithDates, WithTimes, WithTimestamps}, int64(),
      &seconds_between_doc);
  DCHECK_OK(registry->AddFunction(std::move(seconds_between)));

  auto milliseconds_between =
      MakeTemporalBinary<MillisecondsBetween, TemporalBinary, Int64Type>(
          "milliseconds_between", {WithDates, WithTimes, WithTimestamps}, int64(),
          &milliseconds_between_doc);
  DCHECK_OK(registry->AddFunction(std::move(milliseconds_between)));

  auto microseconds_between =
      MakeTemporalBinary<MicrosecondsBetween, TemporalBinary, Int64Type>(
          "microseconds_between", {WithDates, WithTimes, WithTimestamps}, int64(),
          &microseconds_between_doc);
  DCHECK_OK(registry->AddFunction(std::move(microseconds_between)));

  auto nanoseconds_between =
      MakeTemporalBinary<NanosecondsBetween, TemporalBinary, Int64Type>(
          "nanoseconds_between", {WithDates, WithTimes, WithTimestamps}, int64(),
          &nanoseconds_between_doc);
  DCHECK_OK(registry->AddFunction(std::move(nanoseconds_between)));
}

}  // namespace internal
}  // namespace compute
}  // namespace arrow
