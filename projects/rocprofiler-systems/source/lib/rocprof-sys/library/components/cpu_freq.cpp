// MIT License
//
// Copyright (c) 2022-2025 Advanced Micro Devices, Inc. All Rights Reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "library/components/cpu_freq.hpp"
#include "core/common.hpp"
#include "core/components/fwd.hpp"
#include "core/config.hpp"
#include "core/defines.hpp"
#include "core/perfetto.hpp"
#include "core/timemory.hpp"
#include "core/utility.hpp"

#include <timemory/components/macros.hpp>
#include <timemory/components/rusage/backends.hpp>
#include <timemory/mpl/types.hpp>
#include <timemory/units.hpp>
#include <timemory/utility/procfs/cpuinfo.hpp>
#include <timemory/utility/type_list.hpp>

#include "logger/debug.hpp"

namespace cpuinfo = tim::procfs::cpuinfo;

namespace rocprofsys
{
namespace component
{
cpu_freq::cpu_id_set_t&
cpu_freq::get_enabled_cpus()
{
    static auto _v = cpu_id_set_t{};
    return _v;
}

std::string
cpu_freq::label()
{
    return "cpu_freq";
}

std::string
cpu_freq::description()
{
    return "Records the current CPU frequencies";
}

int64_t
cpu_freq::unit()
{
    return tim::units::MHz;
}

std::string
cpu_freq::display_unit()
{
    return tim::units::freq_repr(unit());
}

void
cpu_freq::configure()
{
    auto _ncpu          = cpuinfo::freq::size();
    auto _enabled_freqs = std::set<uint64_t>{};

    auto _enabled_val = get_sampling_cpus();
    for(auto& itr : _enabled_val)
        itr = tolower(itr);
    if(_enabled_val == "off")
        _enabled_val = "none";
    else if(_enabled_val == "on" || _enabled_val.empty())
        _enabled_val = "all";
    bool _all_cpus = _enabled_val.find("all") != std::string::npos;
    bool _no_cpus  = _enabled_val.find("none") != std::string::npos;

    if(_all_cpus)
    {
        for(size_t i = 0; i < _ncpu; ++i)
            _enabled_freqs.emplace(i);
    }
    else if(!_no_cpus)
    {
        auto parsed_cpus = utility::parse_numeric_range<int64_t, std::set<int64_t>>(
            _enabled_val, "CPU", 1L);
        for(auto idx : parsed_cpus)
        {
            if(idx >= 0 && static_cast<uint64_t>(idx) < _ncpu)
            {
                _enabled_freqs.emplace(static_cast<uint64_t>(idx));
            }
            else
            {
                LOG_DEBUG("[cpu_freq::config] Removing invalid CPU {}", idx);
            }
        }
    }

    if(!cpuinfo::freq{})
    {
        LOG_WARNING("[cpu_freq::config] CPU frequencies are disabled "
                    ":: unable to open /proc/cpuinfo");
        _enabled_freqs.clear();
    }

    if(get_is_continuous_integration() && !cpuinfo::freq{})
    {
        LOG_CRITICAL("[cpu_freq::config] CPU frequencies are disabled :: unable to open "
                     "/proc/cpuinfo");
        ::rocprofsys::set_state(::rocprofsys::State::Finalized);
        std::abort();
    }

    get_enabled_cpus() = _enabled_freqs;
}

std::string
cpu_freq::as_string() const
{
    return tim::operation::base_printer<cpu_freq>{}(std::stringstream{}, *this).str();
}

cpu_freq::value_type
cpu_freq::record()
{
    auto& enabled_cpu_freqs = get_enabled_cpus();

    std::vector<uint64_t> _freqs{};
    if(!enabled_cpu_freqs.empty())
    {
        _freqs.reserve(enabled_cpu_freqs.size());
        auto&& _freq = cpuinfo::freq{};
        for(const auto& itr : enabled_cpu_freqs)
        {
            _freqs.emplace_back(_freq(itr) * tim::units::MHz);
        }
    }

    return _freqs;
}

void
cpu_freq::start()
{
    value = record();
}

void
cpu_freq::stop()
{
    using namespace tim::stl;
    value = (record() - value);
}

cpu_freq&
cpu_freq::sample()
{
    value = record();
    return *this;
}

float
cpu_freq::at(size_t _idx, int64_t _unit) const
{
    return (value.at(_idx) / static_cast<float>(_unit));
}

std::vector<float>
cpu_freq::get(int64_t _unit) const
{
    std::vector<float> _v{};
    _v.reserve(value.size());
    for(const auto& itr : value)
        _v.emplace_back(itr / static_cast<float>(_unit));
    return _v;
}
}  // namespace component
}  // namespace rocprofsys

TIMEMORY_INITIALIZE_STORAGE(rocprofsys::component::cpu_freq)
