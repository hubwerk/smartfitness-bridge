#pragma once

#include "debug_log.h"

#if defined(ARDUINO) && defined(DEBUG_WEB)
#include "ant/fec_codec.hpp"

namespace smartfitness::ant::diagnostics {

inline const char* controlName(Control control) {
    switch (control) {
        case Control::Power: return "targetPower";
        case Control::Resistance: return "resistance";
        case Control::Incline: return "incline";
        default: return "speed";
    }
}

inline const char* statusName(fec::CommandStatus status) {
    switch (status) {
        case fec::CommandStatus::Pass: return "pass";
        case fec::CommandStatus::Fail: return "fail";
        case fec::CommandStatus::NotSupported: return "not-supported";
        case fec::CommandStatus::Rejected: return "rejected";
        default: return "none";
    }
}

// Decode only self-contained fields; speed/cadence counters need history.
class Summary {
public:
    Summary(std::uint8_t service, const std::uint8_t* p) {
        if (service == AntFec) {
            Page page{};
            for (std::size_t i = 0; i < page.size(); ++i) page[i] = p[i];
            const auto control = fec::decodeControlPage(page);
            if (control.hasRequest) {
                std::snprintf(text_, sizeof(text_), " %s=%.2f", controlName(control.request.control),
                              static_cast<double>(control.request.value));
                return;
            }
            switch (p[0]) {
                case fec::page::kGeneralFe:
                    std::snprintf(text_, sizeof(text_), " speed=%s km/h hr=%s",
                                  value(word(p + 4), 0xFFFF, 0.0036).c_str(), value(p[6], 0xFF).c_str());
                    break;
                case fec::page::kTrainer:
                    std::snprintf(text_, sizeof(text_), " power=%s W cadence=%s rpm",
                                  value(word(p + 5) & 0xFFF, 0xFFF).c_str(), value(p[2], 0xFF).c_str());
                    break;
                case fec::page::kElliptical:
                case fec::page::kRower:
                case fec::page::kClimber:
                    std::snprintf(text_, sizeof(text_), " power=%s W cadence=%s rpm",
                                  value(word(p + 5), 0xFFFF).c_str(), value(p[4], 0xFF).c_str());
                    break;
                case fec::page::kTreadmill:
                    std::snprintf(text_, sizeof(text_), " cadence=%s rpm", value(p[4], 0xFF).c_str());
                    break;
                case fec::page::kRequest:
                    std::snprintf(text_, sizeof(text_), " requestPage=%u repeats=%u acknowledged=%u command=0x%02x",
                                  p[6], p[5] & 0x7F, p[5] >> 7, p[7]);
                    break;
                case fec::page::kCommandStatus:
                    std::snprintf(text_, sizeof(text_), " commandPage=%u sequence=%u status=%s(0x%02x)",
                                  p[1], p[2], statusName(static_cast<fec::CommandStatus>(p[3])), p[3]);
                    break;
                default: break;
            }
        } else if (service == AntPower && p[0] == 16) {
            std::snprintf(text_, sizeof(text_), " power=%s W cadence=%s rpm",
                          value(word(p + 6), 0xFFFF).c_str(), value(p[3], 0xFF).c_str());
        } else if (service == AntHeartRate) {
            std::snprintf(text_, sizeof(text_), " hr=%s bpm", value(p[7], 0xFF).c_str());
        } else if (service == AntSpeedCadence) {
            std::snprintf(text_, sizeof(text_), " crankTime=%u crankRevs=%u wheelTime=%u wheelRevs=%u",
                          word(p), word(p + 2), word(p + 4), word(p + 6));
        } else if (service == AntStride && p[0] == 1) {
            std::snprintf(text_, sizeof(text_), " speed=%.2f km/h strides=%u",
                          ((p[4] & 15) * 256 + p[5]) * (3.6 / 256), p[6]);
        }
    }
    const char* c_str() const { return text_; }
    static constexpr std::uint8_t AntFec = 1, AntPower = 2, AntHeartRate = 4, AntSpeedCadence = 8, AntStride = 16;

private:
    static unsigned word(const std::uint8_t* p) { return p[0] | (static_cast<unsigned>(p[1]) << 8); }
    static debuglog::Value value(unsigned raw, unsigned invalid, double scale = 1) {
        return debuglog::Value(raw == invalid ? std::nullopt : std::optional<float>(raw * scale));
    }
    char text_[160]{};
};

}  // namespace smartfitness::ant::diagnostics
#endif
