#ifndef __MESYTEC_MVLC_LOGGING_H__
#define __MESYTEC_MVLC_LOGGING_H__

#include <string>
#include <spdlog/spdlog.h>

namespace mesytec
{
namespace mvlc
{

std::vector<std::shared_ptr<spdlog::logger>> setup_loggers(const std::vector<spdlog::sink_ptr> &sinks = {});

template<typename View>
void log_buffer(const std::shared_ptr<spdlog::logger> &logger,
                const spdlog::level::level_enum &level,
                const View &buffer, const std::string &header)
{
    logger->log(level, "begin buffer '{}' (size={})", header, buffer.size());

    for (const auto &value: buffer)
        logger->log(level, "  0x{:008X}", value);


    logger->log(level, "end buffer '{}' (size={})", header, buffer.size());

}

} // end namespace mvlc
} // end namespace mesytec

#endif /* __MESYTEC_MVLC_LOGGING_H__ */
