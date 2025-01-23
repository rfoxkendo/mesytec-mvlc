#include "event_builder2.hpp"

#include <deque>
#include <mesytec-mvlc/util/ticketmutex.h>
#include <spdlog/spdlog.h>

namespace mesytec::mvlc::event_builder2
{

IndexedTimestampFilterExtractor::IndexedTimestampFilterExtractor(const util::DataFilter &filter,
                                                                 s32 wordIndex, char matchChar)
    : filter_(filter)
    , filterCache_(make_cache_entry(filter_, matchChar))
    , index_(wordIndex)
{
}

std::optional<u32> IndexedTimestampFilterExtractor::operator()(const u32 *data, size_t size)
{
    size_t idx = index_ < 0 ? size + index_ : index_;

    if (static_cast<size_t>(idx) < size && matches(filter_, data[idx]))
        return extract(filterCache_, data[idx]);

    return {};
}

TimestampFilterExtractor::TimestampFilterExtractor(const util::DataFilter &filter, char matchChar)
    : filter_(filter)
    , filterCache_(make_cache_entry(filter_, matchChar))
{
}

std::optional<u32> TimestampFilterExtractor::operator()(const u32 *data, size_t size)
{
    for (const u32 *valuep = data; valuep < data + size; ++valuep)
    {
        if (matches(filter_, *valuep))
            return extract(filterCache_, *valuep);
    }

    return {};
}

u32 add_offset_to_timestamp(u32 ts, s32 offset)
{
    return (ts + offset) & TimestampMax; // Adjust and wrap around within 30-bit range
}

s64 timestamp_difference(s64 ts0, s64 ts1)
{
    s64 diff = ts0 - ts1;

    if (std::abs(diff) > TimestampHalf)
    {
        // overflow handling
        if (diff < 0)
            diff += TimestampMax;
        else
            diff -= TimestampMax;
    }

    return diff;
}

WindowMatchResult timestamp_match(s64 tsMain, s64 tsModule, u32 windowWidth)
{
    s64 diff = timestamp_difference(tsMain, tsModule);

    if (std::abs(diff) > windowWidth * 0.5)
    {
        if (diff >= 0)
            return {WindowMatch::too_old, static_cast<u32>(std::abs(diff))};
        else
            return {WindowMatch::too_new, static_cast<u32>(std::abs(diff))};
    }

    return {WindowMatch::in_window, static_cast<u32>(std::abs(diff))};
}

using TimestampType = s64;

struct ModuleStorage
{
    std::vector<u32> data;
    u32 prefixSize;
    u32 dynamicSize;
    u32 suffixSize;
    bool hasDynamic;
    std::optional<TimestampType> timestamp;

    ModuleStorage(const ModuleData &md = {}, std::optional<TimestampType> ts = {})
        : data(md.data.data, md.data.data + md.data.size)
        , prefixSize(md.prefixSize)
        , dynamicSize(md.dynamicSize)
        , suffixSize(md.suffixSize)
        , hasDynamic(md.hasDynamic)
        , timestamp(ts)
    {
    }

    ModuleData to_module_data() const
    {
        return {
            .data = {data.data(), static_cast<u32>(data.size())},
            .prefixSize = prefixSize,
            .dynamicSize = dynamicSize,
            .suffixSize = suffixSize,
            .hasDynamic = hasDynamic,
        };
    }
};

inline bool size_consistency_check(const ModuleStorage &md)
{
    u64 partSum = md.prefixSize + md.dynamicSize + md.suffixSize;
    bool sumOk = partSum == md.data.size();
    // Note: cannot test the opposite: the current dynamicSize can be 0 but
    // hasDynamic can be true at the same time, e.g. from empty block reads.
    bool dynOk = md.dynamicSize > 0 ? md.hasDynamic : true;
    return sumOk && dynOk;
}

struct PerEventData
{

    // Timestamps of all modules of the incoming event are stored here.
    std::deque<TimestampType> allTimestamps;
    // Module data and extracted timestamps are stored here.
    std::vector<std::deque<ModuleStorage>> moduleDatas;
    // delta timestamp histograms
    std::vector<Histo> dtHistograms;
};

// Records module data and unmodified timestamps.
inline bool record_module_data(const ModuleData *moduleDataList, unsigned moduleCount,
                               const std::vector<ModuleConfig> &cfgs,
                               std::vector<std::deque<ModuleStorage>> &dest,
                               EventCounters &counters)
{
    assert(cfgs.size() == moduleCount);
    assert(dest.size() == moduleCount);
    assert(std::all_of(moduleDataList, moduleDataList + moduleCount,
                       [](const ModuleData &md)
                       { return mvlc::readout_parser::size_consistency_check(md); }));

    if (cfgs.size() != moduleCount)
        return false;

    if (dest.size() != moduleCount)
        return false;

    for (unsigned mi = 0; mi < moduleCount; ++mi)
    {
        const auto &mdata = moduleDataList[mi];
        const auto &mcfg = cfgs[mi];
        auto ts = mcfg.tsExtractor(mdata.data.data, mdata.data.size);

        ++counters.inputHits[mi];

        if (mdata.data.size == 0)
            ++counters.emptyInputs[mi];

        if (!mcfg.ignored && !ts.has_value() && mdata.data.size > 0)
        {
            ++counters.stampFailed[mi];
            spdlog::trace(
                "record_module_data: failed timestamp extraction, module{}, data.size={}, "
                "data={:#010x}",
                mi, mdata.data.size,
                fmt::join(mdata.data.data, mdata.data.data + mdata.data.size, ", "));
        }

        dest[mi].emplace_back(ModuleStorage(mdata, ts));

        ++counters.currentEvents[mi];
        counters.currentMem[mi] += mdata.data.size * sizeof(u32);
        counters.maxEvents[mi] = std::max(counters.maxEvents[mi], counters.currentEvents[mi]);
        counters.maxMem[mi] = std::max(counters.maxMem[mi], counters.currentMem[mi]);
    }

    return true;
}

std::string dump_counters(const EventCounters &counters)
{
    std::ostringstream oss;

    std::vector<size_t> sumOutputsDiscards(counters.outputHits.size());
    std::transform(std::begin(counters.outputHits), std::end(counters.outputHits),
                   std::begin(counters.discardsAge), std::begin(sumOutputsDiscards),
                   std::plus<size_t>());

    oss << fmt::format("inputHits:          {}\n", fmt::join(counters.inputHits, ", "));
    oss << fmt::format("discardsAge:        {}\n", fmt::join(counters.discardsAge, ", "));
    oss << fmt::format("outputHits:         {}\n", fmt::join(counters.outputHits, ", "));
    oss << fmt::format("sumOutputsDiscards: {}\n", fmt::join(sumOutputsDiscards, ", "));
    oss << fmt::format("emptyInputs:        {}\n", fmt::join(counters.emptyInputs, ", "));
    oss << fmt::format("stampFailed:        {}\n", fmt::join(counters.stampFailed, ", "));

    oss << fmt::format("currentEvents:      {}\n", fmt::join(counters.currentEvents, ", "));
    oss << fmt::format("maxEvents:          {}\n", fmt::join(counters.maxEvents, ", "));

    oss << fmt::format("currentMem:         {}\n", fmt::join(counters.currentMem, ", "));
    oss << fmt::format("maxMem:             {}\n", fmt::join(counters.maxMem, ", "));

    return oss.str();
}

bool fill(Histo &histo, double x)
{
    if (x < histo.xMin)
    {
        ++histo.underflows;
    }
    else if (x >= histo.xMax)
    {
        ++histo.overflows;
    }
    else
    {
        size_t bin =
            static_cast<size_t>((x - histo.xMin) / (histo.xMax - histo.xMin) * histo.bins.size());
        assert(bin < histo.bins.size());
        if (bin < histo.bins.size())
        {
            ++histo.bins[bin];
            return true;
        }
    }

    return false;
}

struct EventBuilder2::Private
{
    EventBuilderConfig cfg_;
    Callbacks callbacks_;
    void *userContext_;
    std::vector<PerEventData> perEventData_;
    // Used for the callback interface which requires a flat ModuleData array.
    std::vector<ModuleData> outputModuleData_;
    std::vector<ModuleStorage> outputModuleStorage_;

    mvlc::TicketMutex mutex_;
    BuilderCounters counters_;

    bool checkConsistency(int eventIndex, const ModuleData *moduleDataList, unsigned moduleCount)
    {
        auto a = (0 <= eventIndex && static_cast<size_t>(eventIndex) < cfg_.eventConfigs.size());
        auto b = cfg_.eventConfigs.size() == perEventData_.size();
        auto c = counters_.eventCounters.size() == perEventData_.size();

        if (a && b && c)
        {
            return std::all_of(moduleDataList, moduleDataList + moduleCount,
                               [](const ModuleData &md)
                               { return mvlc::readout_parser::size_consistency_check(md); });
        }

        return false;
    }

    bool checkModuleBuffers(int eventIndex)
    {
        auto a = (0 <= eventIndex && static_cast<size_t>(eventIndex) < cfg_.eventConfigs.size());
        auto b = cfg_.eventConfigs.size() == perEventData_.size();
        auto c = counters_.eventCounters.size() == perEventData_.size();

        if (!(a && b && c))
            return false;

        auto &eventData = perEventData_[eventIndex];
        auto moduleCount = eventData.moduleDatas.size();

        bool result = true;

        for (unsigned mi = 0; mi < moduleCount; ++mi)
        {
            auto &mds = eventData.moduleDatas[mi];

            result = result && std::all_of(std::begin(mds), std::end(mds),
                                           [](const ModuleStorage &md)
                                           { return size_consistency_check(md); });

            // This will fail for the extreme case where none of the modules in
            // an event yielded a timestamp. fillerTs in recordModuleData() will
            // not be set and the otherwise guaranteed stamp will not be
            // appended to the queue.
            result = result &&
                     std::all_of(std::begin(mds), std::end(mds),
                                 [](const ModuleStorage &md) { return md.timestamp.has_value(); });
        }

        return result;
    }

    bool recordModuleData(int eventIndex, const ModuleData *moduleDataList, unsigned moduleCount)
    {
        spdlog::trace("entering recordModuleData: eventIndex={}, moduleCount={}", eventIndex,
                      moduleCount);

        if (!checkConsistency(eventIndex, moduleDataList, moduleCount))
        {
            spdlog::warn("recordModuleData: eventIndex={}, moduleCount={} -> module data "
                         "consistency check failed",
                         eventIndex, moduleCount);
            return false;
        }

        auto &eventData = perEventData_[eventIndex];
        auto &eventCfg = cfg_.eventConfigs[eventIndex];
        auto &eventCtrs = counters_.eventCounters[eventIndex];

        if (!eventCfg.enabled)
        {
            callbacks_.eventData(userContext_, cfg_.outputCrateIndex, eventIndex, moduleDataList,
                                 moduleCount);

            for (unsigned mi = 0; mi < moduleCount; ++mi)
            {
                ++eventCtrs.inputHits[mi];
                ++eventCtrs.outputHits[mi];
            }

            return true;
        }

        // Record incoming module data and extracted timestamps.
        // If it returns false none of the ModuleStorages haven been modified.
        // Otherwise all of them have a new entry even if no timestamp could be extracted.
        if (record_module_data(moduleDataList, moduleCount, eventCfg.moduleConfigs,
                               eventData.moduleDatas, eventCtrs))
        {
            // The back of each 'moduleDatas' queue now contains the newest data+timestamp.

#if 0
            auto &histos = eventData.dtHistograms;
            auto itHistos = std::begin(histos);

            for (unsigned mi = 0; mi < moduleCount; ++mi)
            {
                for (unsigned mi2 = 0; mi2 < moduleCount; ++mi2)
                {
                    assert(itHistos != std::end(histos));

                    auto &md = eventData.moduleDatas[mi].back();
                    auto &md2 = eventData.moduleDatas[mi2].back();

                    if (md.timestamp.has_value() && md2.timestamp.has_value())
                    {
                        s64 dt = timestamp_difference(md.timestamp.value(), md2.timestamp.value());
                        fill(*itHistos, dt);
                    }

                    ++itHistos;
                }
            }
#endif

            // Apply offsets to the timestamps.
            for (unsigned mi = 0; mi < moduleCount; ++mi)
            {
                auto &ms = eventData.moduleDatas[mi].back();
                if (ms.timestamp.has_value())
                {
                    *ms.timestamp =
                        add_offset_to_timestamp(*ms.timestamp, eventCfg.moduleConfigs[mi].offset);
                }
            }

            // The filler stamp is used for modules that do not yield a valid
            // stamp. Makes flushing easier and keeps non-stamped modules together
            // with their sister modules on output.
            std::optional<TimestampType> fillerTs;

            for (unsigned mi = 0; mi < moduleCount; ++mi)
            {
                if (auto ts = eventData.moduleDatas[mi].back().timestamp; ts.has_value())
                {
                    eventData.allTimestamps.push_back(*ts);

                    if (!fillerTs.has_value())
                    {
                        fillerTs = ts;
                        spdlog::trace(
                            "recordModuleData: eventIndex={}, moduleIndex={} -> set fillerTs={}",
                            eventIndex, mi, fillerTs.value());
                    }
                }
            }

            if (fillerTs.has_value())
            {
                for (unsigned mi = 0; mi < moduleCount; ++mi)
                {
                    if (!eventData.moduleDatas[mi].back().timestamp.has_value())
                    {
                        eventData.moduleDatas[mi].back().timestamp = fillerTs;
                        spdlog::trace("recordModuleData: eventIndex={}, moduleIndex={} -> assign "
                                      "fillerTs={}, data.size={}",
                                      eventIndex, mi, fillerTs.value(),
                                      eventData.moduleDatas[mi].back().data.size());
                    }
                    else
                    {
                        spdlog::trace("recordModuleData: eventIndex={}, moduleIndex={} -> module "
                                      "has valid ts, ts={}, data.size={}",
                                      eventIndex, mi,
                                      eventData.moduleDatas[mi].back().timestamp.value(),
                                      eventData.moduleDatas[mi].back().data.size());
                    }
                }
            }
            else
            {
                spdlog::trace("recordModuleData: eventIndex={} -> no fillerTs available",
                              eventIndex);
            }

            spdlog::trace("leaving recordModuleData: eventIndex={}, moduleCount={} -> return true",
                          eventIndex, moduleCount);

            return true;
        }

        spdlog::warn("leaving recordModuleData: eventIndex={}, moduleCount={} -> return false",
                     eventIndex, moduleCount);

        ++eventCtrs.recordingFailed;
        return false;
    }

    bool tryFlush(int eventIndex)
    {
        spdlog::trace("entering tryFlush: eventIndex={}", eventIndex);

        if (!checkModuleBuffers(eventIndex))
        {
            spdlog::trace("tryFlush: eventIndex={} -> checkModuleBuffers failed -> return false",
                          eventIndex);
            return false;
        }

        auto &eventData = perEventData_[eventIndex];
        auto &eventCfg = cfg_.eventConfigs[eventIndex];
        auto &eventCtrs = counters_.eventCounters[eventIndex];

        if (!eventCfg.enabled)
            return false;

        const auto moduleCount = eventCfg.moduleConfigs.size();
        const auto refTs = eventData.allTimestamps.front();

        // check if the latest timestamps of all modules are "in the future",
        // e.g. too new to be in the match window of the current reference
        // stamp.
        for (size_t mi = 0; mi < moduleCount; ++mi)
        {
            auto &mc = eventCfg.moduleConfigs[mi];

            // no data in the queue for this module
            if (eventData.moduleDatas.at(mi).empty())
                continue;

            auto modTs = eventData.moduleDatas.at(mi).back().timestamp.value();
            auto matchResult = timestamp_match(refTs, modTs, mc.window);
            if (matchResult.match != WindowMatch::too_new)
            {
                spdlog::trace("tryFlush: module{}, refTs={}, modTs={}, window={}, match={} -> "
                              "newest stamp is not far enough in the future, cannot flush yet -> "
                              "return false",
                              mi, refTs, modTs, mc.window, (int)matchResult.match);
                return false;
            }
        }

        spdlog::trace(
            "tryFlush: refTs={}, all modules have a ts in the future -> flushing at least "
            "one event",
            refTs);

        // pop the refTs, so we won't encounter it again. Loop because multiple modules might yield
        // the exact same refTs.
        while (!eventData.allTimestamps.empty() && eventData.allTimestamps.front() == refTs)
        {
            eventData.allTimestamps.pop_front();
        }

        // pop data that is too old and thus can never be matched
        for (size_t mi = 0; mi < moduleCount; ++mi)
        {
            auto &moduleDatas = eventData.moduleDatas.at(mi);
            auto &moduleConfig = eventCfg.moduleConfigs.at(mi);

            while (!moduleDatas.empty())
            {
                auto modTs = moduleDatas.front().timestamp.value();
                auto matchResult = timestamp_match(refTs, modTs, moduleConfig.window);

                if (matchResult.match == WindowMatch::too_old)
                {
                    spdlog::trace("  tryFlush: mi={}, refTs={}, modTs={}, window={}, too_old -> "
                                  "discard event",
                                  mi, refTs, modTs, moduleConfig.window);
                    ++eventCtrs.discardsAge[mi];
                    --eventCtrs.currentEvents[mi];
                    eventCtrs.currentMem[mi] -= moduleDatas.front().data.size() * sizeof(u32);
                    moduleDatas.pop_front();
                }
                else
                {
                    break;
                }
            }
        }

        outputModuleStorage_.resize(moduleCount);

        for (size_t mi = 0; mi < moduleCount; ++mi)
        {
            auto &moduleConfig = eventCfg.moduleConfigs.at(mi);
            auto &moduleDatas = eventData.moduleDatas.at(mi);

            outputModuleStorage_[mi] = {};
            // Set attributes from the config and resize data in case we do not
            // actually get real input data for this module.
            outputModuleStorage_[mi].prefixSize = moduleConfig.prefixSize;
            outputModuleStorage_[mi].hasDynamic = moduleConfig.hasDynamic;
            outputModuleStorage_[mi].data.resize(moduleConfig.prefixSize);
            std::fill(std::begin(outputModuleStorage_[mi].data),
                      std::end(outputModuleStorage_[mi].data), 0);

            while (!moduleDatas.empty())
            {
                auto modTs = moduleDatas.front().timestamp.value();
                auto matchResult = timestamp_match(refTs, modTs, moduleConfig.window);

                assert(matchResult.match != WindowMatch::too_old);
                s64 dt = refTs - modTs;

                if (matchResult.match == WindowMatch::in_window)
                {
                    spdlog::trace("  tryFlush: mi={}, refTs={}, modTs={}, dt={}, window={}, "
                                  "in_window -> add to out event",
                                  mi, refTs, modTs, dt, moduleConfig.window);
                    // Move data to the output buffer. Needed for the linear ModuleData array.
                    outputModuleStorage_[mi] = std::move(moduleDatas.front());
                    moduleDatas.pop_front();
                    ++eventCtrs.outputHits[mi];
                    --eventCtrs.currentEvents[mi];
                    eventCtrs.currentMem[mi] -= outputModuleStorage_[mi].data.size() * sizeof(u32);
                    break;
                }
                else if (matchResult.match == WindowMatch::too_new)
                {
                    spdlog::trace(
                        "  tryFlush: mi={}, refTs={}, modTs={}, dt={}, window={}, too_new "
                        "-> leave in buffer",
                        mi, refTs, modTs, dt, moduleConfig.window);
                    break;
                }
            }
        }

        std::vector<std::string> debugStamps;
        for (size_t mi = 0; mi < moduleCount; ++mi)
        {
            if (outputModuleStorage_[mi].timestamp.has_value())
            {
                debugStamps.push_back(
                    fmt::format("{}", outputModuleStorage_[mi].timestamp.value()));
            }
            else
            {
                debugStamps.push_back("n/a");
            }
        }

        spdlog::trace("tryFlush: eventIndex={}, refTs={}, outputStamps={}", eventIndex, refTs,
                      fmt::join(debugStamps, ", "));

        outputModuleData_.resize(moduleCount);
        for (size_t mi = 0; mi < moduleCount; ++mi)
        {
            auto &mcfg = eventCfg.moduleConfigs.at(mi);

            if (!size_consistency_check(outputModuleStorage_[mi]))
            {
                spdlog::error("  tryFlush: mi={}, size_consistency_check failed", mi);
            }
            assert(size_consistency_check(outputModuleStorage_[mi]));
            outputModuleData_[mi] = outputModuleStorage_[mi].to_module_data();
            assert(mvlc::readout_parser::size_consistency_check(outputModuleData_[mi]));
        }

        callbacks_.eventData(userContext_, cfg_.outputCrateIndex, eventIndex,
                             outputModuleData_.data(), moduleCount);

        return true;
    }

    size_t forceFlush(int eventIndex)
    {
        spdlog::trace("entering forceFlush: eventIndex={}", eventIndex);
        auto &ed = perEventData_.at(eventIndex);
        bool haveData = false;
        size_t result = 0;
        const auto moduleCount = ed.moduleDatas.size();
        outputModuleData_.resize(moduleCount);
        auto &eventCtrs = counters_.eventCounters[eventIndex];

        do
        {
            haveData = false;
            for (size_t mi = 0; mi < moduleCount; ++mi)
            {
                auto &mds = ed.moduleDatas.at(mi);
                if (mds.empty())
                {
                    outputModuleData_[mi] = {};
                    continue;
                }
                // Move data to the output buffer. Needed for the linear ModuleData array.
                outputModuleStorage_[mi] = std::move(mds.front());
                mds.pop_front();
                outputModuleData_[mi] = outputModuleStorage_[mi].to_module_data();
                ++eventCtrs.outputHits[mi];
                --eventCtrs.currentEvents[mi];
                eventCtrs.currentMem[mi] -= outputModuleStorage_[mi].data.size() * sizeof(u32);
                haveData = true;
            }
            if (haveData)
            {
                callbacks_.eventData(userContext_, cfg_.outputCrateIndex, eventIndex,
                                     outputModuleData_.data(), moduleCount);
                ++result;
            }
        }
        while (haveData);

        spdlog::trace("leaving forceFlush: eventIndex={} -> flushed {} events", eventIndex, result);

        return result;
    }
};

template <typename T> void resize_and_clear(size_t size, T &t)
{
    t.resize(size);
    std::fill(std::begin(t), std::end(t), typename T::value_type{});
}

template <typename... Ts> void resize_and_clear(size_t size, Ts &&...args)
{
    (resize_and_clear(size, args), ...);
}

EventBuilder2::EventBuilder2(const EventBuilderConfig &cfg, Callbacks callbacks, void *userContext)
    : d(std::make_unique<Private>())
{
    for (size_t ei = 0; ei < cfg.eventConfigs.size(); ++ei)
    {
        const auto &ecfg = cfg.eventConfigs.at(ei);
        for (size_t mi = 0; mi < ecfg.moduleConfigs.size(); ++mi)
        {
            const auto &mcfg = ecfg.moduleConfigs.at(mi);
            if (!mcfg.hasDynamic && mcfg.prefixSize == 0)
            {
                throw std::runtime_error(
                    fmt::format("EventBuilder2: config error: eventIndex={}, moduleIndex={} -> "
                                "static prefix size must be set if hasDynamic==false",
                                ei, mi));
            }
        }
    }

    d->cfg_ = cfg;
    d->callbacks_ = callbacks;
    d->userContext_ = userContext;
    d->perEventData_.resize(cfg.eventConfigs.size());
    d->counters_.eventCounters.resize(cfg.eventConfigs.size());

    // Initialize counters
    for (size_t ei = 0; ei < cfg.eventConfigs.size(); ++ei)
    {
        auto &ec = cfg.eventConfigs.at(ei);
        auto &ed = d->perEventData_.at(ei);
        auto &ctrs = d->counters_.eventCounters.at(ei);
        resize_and_clear(ec.moduleConfigs.size(), ed.moduleDatas, ctrs.inputHits, ctrs.outputHits,
                         ctrs.emptyInputs, ctrs.discardsAge, ctrs.stampFailed, ctrs.currentEvents,
                         ctrs.currentMem, ctrs.maxEvents, ctrs.maxMem);
    }

// Create dtHistograms
#if 0
    for (size_t ei = 0; ei < cfg.eventConfigs.size(); ++ei)
    {
        auto &ec = cfg.eventConfigs.at(ei);
        auto &ed = d->perEventData_.at(ei);

        if (!ec.enabled)
            continue;

        for (size_t mi=0; mi<ec.moduleConfigs.size(); ++mi)
        {
            auto &mc1 = ec.moduleConfigs[mi];
            for (size_t mi2=0; mi2<ec.moduleConfigs.size(); ++mi2)
            {
                auto &mc2 = ec.moduleConfigs[mi2];
                Histo histo{};
                // TODO: make the histo parameters configurable with reasonable default values.
                // 1 timestamp clock tick == 62.5 ns
                histo.xMin = -32;
                histo.xMax = +32;
                histo.bins = std::vector<size_t>(histo.xMax - histo.xMin, 0);
                histo.title = fmt::format("dt({}, {})", mc1.name, mc2.name);
                ed.dtHistograms.emplace_back(std::move(histo));
            }
        }
    }
#endif
}

EventBuilder2::EventBuilder2(const EventBuilderConfig &cfg, void *userContext)
    : EventBuilder2(cfg, {}, userContext)
{
}

EventBuilder2::EventBuilder2()
    : EventBuilder2({}, {}, nullptr)
{
}

EventBuilder2::EventBuilder2(EventBuilder2 &&) = default;
EventBuilder2 &EventBuilder2::operator=(EventBuilder2 &&) = default;

EventBuilder2::~EventBuilder2() {}

void EventBuilder2::setCallbacks(const Callbacks &callbacks) { d->callbacks_ = callbacks; }

bool EventBuilder2::recordModuleData(int eventIndex, const ModuleData *moduleDataList,
                                     unsigned moduleCount)
{
    std::unique_lock<mvlc::TicketMutex> guard(d->mutex_);
    if (0 > eventIndex || static_cast<size_t>(eventIndex) >= d->perEventData_.size())
    {
        // TODO: count EventIndexOutOfRange
        return false;
    }

    d->recordModuleData(eventIndex, moduleDataList, moduleCount);
    return true;
}

void EventBuilder2::handleSystemEvent(const u32 *header, u32 size)
{
    std::unique_lock<mvlc::TicketMutex> guard(d->mutex_);
    d->callbacks_.systemEvent(d->userContext_, d->cfg_.outputCrateIndex, header, size);
}

size_t EventBuilder2::flush(bool force)
{
    std::unique_lock<mvlc::TicketMutex> guard(d->mutex_);
    size_t flushed = 0;

    if (force)
    {
        for (size_t eventIndex = 0; eventIndex < d->perEventData_.size(); ++eventIndex)
        {
            flushed += d->forceFlush(eventIndex);
        }
    }
    else
    {

        for (size_t eventIndex = 0; eventIndex < d->perEventData_.size(); ++eventIndex)
        {
            // fmt::print("pre tryFlush: {}\n", debugDump());
            while (d->tryFlush(eventIndex))
            {
                ++flushed;
            }
            // if (flushed > 0)
            //     fmt::print("post tryFlush: {}\n", debugDump());
        }
    }

    return flushed;
}

std::string EventBuilder2::debugDump() const
{
    std::unique_lock<mvlc::TicketMutex> guard(d->mutex_);
    std::string result;

    for (size_t ei = 0; ei < d->perEventData_.size(); ++ei)
    {
        result += fmt::format("Event {}:\n", ei);
        auto &ed = d->perEventData_.at(ei);

        auto stampsToPrint = std::min(static_cast<size_t>(10), ed.allTimestamps.size());
        auto stampsBegin = std::begin(ed.allTimestamps);
        auto stampsEnd = stampsBegin;
        std::advance(stampsEnd, stampsToPrint);

        result += fmt::format("  First {} timestamps of {}: {}\n", stampsToPrint,
                              ed.allTimestamps.size(), fmt::join(stampsBegin, stampsEnd, ", "));

        for (size_t mi = 0; mi < ed.moduleDatas.size(); ++mi)
        {
            auto window = d->cfg_.eventConfigs.at(ei).moduleConfigs.at(mi).window;
            auto &moduleDatas = ed.moduleDatas.at(mi);
            auto stampsToPrint = std::min(static_cast<size_t>(10), moduleDatas.size());
            auto stampsBegin = std::begin(moduleDatas);
            auto stampsEnd = stampsBegin;
            std::advance(stampsEnd, stampsToPrint);
            std::vector<std::string> stamps;
            std::transform(stampsBegin, stampsEnd, std::back_inserter(stamps),
                           [](const ModuleStorage &md) {
                               return md.timestamp.has_value()
                                          ? std::to_string(md.timestamp.value())
                                          : "no ts";
                           });

            result += fmt::format(
                "  Module {}, bufferedEvents={}, window={}, first {} timestamps of {}: {}\n", mi,
                moduleDatas.size(), window, stampsToPrint, stamps.size(), fmt::join(stamps, ", "));
        }
    }
    return result;
}

bool EventBuilder2::isEnabledForAnyEvent() const
{
    for (const auto &ec: d->cfg_.eventConfigs)
    {
        if (ec.enabled)
            return true;
    }
    return false;
}

BuilderCounters EventBuilder2::getCounters() const
{
    std::unique_lock<mvlc::TicketMutex> guard(d->mutex_);
    return d->counters_;
}

std::vector<std::vector<Histo>> EventBuilder2::getAllDtHistograms() const
{
    std::unique_lock<mvlc::TicketMutex> guard(d->mutex_);
    std::vector<std::vector<Histo>> result;

    for (const auto &ed: d->perEventData_)
    {
        result.emplace_back(ed.dtHistograms);
    }

    return result;
}

} // namespace mesytec::mvlc::event_builder2
