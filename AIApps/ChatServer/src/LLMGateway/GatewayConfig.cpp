#include "../../include/LLMGateway/GatewayConfig.h"
#include <fstream>
#include <iostream>

bool GatewayConfig::loadFromFile(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "[GatewayConfig] Cannot open config file: " << path
                  << ", gateway disabled" << std::endl;
        enabled = false;
        return false;
    }

    try {
        json cfg = json::parse(f);

        // ---- enabled ----
        enabled = cfg.value("enabled", true);
        if (!enabled) {
            std::cout << "[GatewayConfig] Gateway is disabled" << std::endl;
            return true;
        }

        // ---- backends ----
        if (cfg.contains("backends")) {
            for (auto& [id, be] : cfg["backends"].items()) {
                BackendConfig bc;
                bc.id        = id;
                bc.apiUrl    = be.value("api_url", "");
                bc.apiKey    = be.value("api_key", "");
                bc.modelName = be.value("model_name", "");
                backends[id] = bc;
                std::cout << "[GatewayConfig] Backend: " << id
                          << " -> " << bc.modelName << std::endl;
            }
        }

        // ---- rate_limit ----
        if (cfg.contains("rate_limit")) {
            auto& rl = cfg["rate_limit"];

            if (rl.contains("per_backend")) {
                for (auto& [id, entry] : rl["per_backend"].items()) {
                    RateLimitEntry e;
                    e.requestsPerSecond = entry.value("requests_per_second", 10.0);
                    e.burstSize         = entry.value("burst_size", 5.0);
                    backendRateLimits[id] = e;
                }
            }

            if (rl.contains("per_user")) {
                auto& pu = rl["per_user"];
                userRps   = pu.value("requests_per_second", 3.0);
                userBurst = pu.value("burst_size", 2.0);
            }
        }

        // ---- circuit_breaker ----
        if (cfg.contains("circuit_breaker")) {
            auto& cb = cfg["circuit_breaker"];
            cbFailureThreshold  = cb.value("failure_threshold", 5);
            cbRecoveryTimeoutMs = cb.value("recovery_timeout_ms", 30000LL);
            cbHalfOpenMax       = cb.value("half_open_max_requests", 3);
        }

        // ---- timeout ----
        if (cfg.contains("timeout")) {
            auto& to = cfg["timeout"];
            connectTimeoutMs = to.value("connect_timeout_ms", 5000LL);
            requestTimeoutMs = to.value("request_timeout_ms", 60000LL);
        }

        // ---- routing ----
        if (cfg.contains("routing")) {
            for (auto& [id, entry] : cfg["routing"].items()) {
                std::vector<std::string> chain;
                if (entry.contains("fallback")) {
                    for (auto& fb : entry["fallback"]) {
                        chain.push_back(fb.get<std::string>());
                    }
                }
                routing[id] = chain;
                std::cout << "[GatewayConfig] Route: " << id;
                for (auto& fb : chain) std::cout << " -> " << fb;
                std::cout << std::endl;
            }
        }

        validate();
        std::cout << "[GatewayConfig] Loaded successfully" << std::endl;
        return true;

    } catch (const std::exception& e) {
        std::cerr << "[GatewayConfig] Parse error: " << e.what()
                  << ", gateway disabled" << std::endl;
        enabled = false;
        return false;
    }
}

void GatewayConfig::validate() const {
    for (auto& [id, chain] : routing) {
        for (auto& fb : chain) {
            if (backends.find(fb) == backends.end()) {
                std::cerr << "[GatewayConfig] WARNING: fallback '" << fb
                          << "' referenced in routing for '" << id
                          << "' but not defined in backends" << std::endl;
            }
        }
    }
}
