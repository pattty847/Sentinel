#include "Roller.hpp"
#include "ConfigLoader.hpp"
#include <yaml-cpp/yaml.h>
#include <iostream>
#include <sstream>
namespace sentinel::roller {
int rollMain(int argc,char** argv) {
    try {
        if (argc < 3) throw std::runtime_error("usage: sentinel-roll JOURNAL_ROOT HMC2_ROOT --products BTC-USD,PEPE-USD --from YYYY-MM-DD --to YYYY-MM-DD [--config YAML] [--dry-run] [--product-lease]");
        RollOptions o; o.journalRoot=argv[1]; o.outputRoot=argv[2];
        std::vector<std::string> products; YAML::Node config;
        for (int i=3;i<argc;++i) {
            const std::string option=argv[i];
            if (option=="--dry-run" || option=="--report") { o.dryRun=true; continue; }
            // Shared root lease plus an exclusive per-product lease (INV-115): may
            // write a root a live roller serves, never a product it is rolling.
            if (option=="--product-lease") { o.productWriterLease=true; continue; }
            if (i+1==argc) throw std::runtime_error("missing value: "+option);
            const std::string value=argv[++i];
            if (option=="--from") o.fromMs=parseTime(value);
            else if (option=="--to") o.toMs=parseTime(value);
            else if (option=="--products") { std::stringstream in(value); std::string p; while(std::getline(in,p,',')) products.push_back(p); }
            else if (option=="--config") config=YAML::LoadFile(value);
            else throw std::runtime_error("unknown option: "+option);
        }
        if (products.empty()) throw std::runtime_error("--products is required");
        const auto source=std::filesystem::weakly_canonical(o.journalRoot).string()+"/";
        const auto dest=std::filesystem::weakly_canonical(o.outputRoot).string()+"/";
        if (source.starts_with(dest) || dest.starts_with(source)) throw std::runtime_error("journal and output roots must be disjoint");
        // Load the server's standard config even when a separate overrides file
        // was supplied. An override must never bypass the live-root exclusion.
        const auto refuseLiveRoot = [&](const YAML::Node& server) {
            if (!server || !server["recording"]) return;
            const auto refuse = [&](const YAML::Node& value) {
                if (!value || value.as<std::string>().empty()) return;
                const auto live = std::filesystem::weakly_canonical(value.as<std::string>()).string()+"/";
                if (dest.starts_with(live) || live.starts_with(dest))
                    throw std::runtime_error("refusing configured live recording root: " + live);
            };
            for (const auto* key : {"dir", "fallback_dir"}) refuse(server["recording"][key]);
            // A roller-served root is live too; only product-scoped repair may write it.
            const auto source = server["recording"]["source"];
            if (source && source.as<std::string>() == "roller" && !o.productWriterLease && server["roller_shadow"])
                refuse(server["roller_shadow"]["dir"]);
        };
        bool foundServerConfig = false;
        ServerConfig effective; // the server's own merge: the private override wins per key
        for (const auto* path : {"config/server_config.yaml", "config/.server_config.yaml"})
            if (std::filesystem::exists(path)) {
                refuseLiveRoot(YAML::LoadFile(path)); foundServerConfig = true;
                if (!ConfigLoader::loadServerConfig(path, &effective))
                    throw std::runtime_error(std::string("server config unreadable: ") + path);
            }
        if (foundServerConfig && effective.recording.source == "roller" && !o.productWriterLease &&
            !effective.rollerShadow.outputRoot.empty()) {
            const auto live = std::filesystem::weakly_canonical(effective.rollerShadow.outputRoot).string()+"/";
            if (dest.starts_with(live) || live.starts_with(dest))
                throw std::runtime_error("refusing roller-served recording root: " + live);
        }
        if (!foundServerConfig && !config)
            throw std::runtime_error("server config unavailable; run from the checkout or supply --config with recording.dir");
        if (!foundServerConfig && (!config["recording"] || !config["recording"]["dir"]))
            throw std::runtime_error("--config must specify recording.dir when the standard server config is unavailable");
        refuseLiveRoot(config);
        for (const auto& p : products) validateOutputProduct(o.outputRoot, p);
        nlohmann::json reports=nlohmann::json::array();
        for (const auto& p:products) {
            o.product=p; o.overrides=nlohmann::json::object();
            if (config && config["recording"] && config["recording"]["products"] && config["recording"]["products"][p]) {
                const auto c=config["recording"]["products"][p];
                for (const auto* key:{"near_tick","deep_tick","price_scale","size_floor"}) if(c[key]) o.overrides[key]=c[key].as<double>();
            }
            reports.push_back(roll(o));
        }
        std::cout << reports.dump(2) << '\n'; return 0;
    } catch (const std::exception& e) { std::cerr << "sentinel-roll: " << e.what() << '\n'; return 1; }
}
int diffMain(int argc,char** argv) {
    try {
        if (argc<7 || argc>8) throw std::runtime_error("usage: hmc2_diff ROOT_A ROOT_B PRODUCT near|deep FROM TO [60000|3600000]");
        const auto report=diff(argv[1],argv[2],argv[3],argv[4],parseTime(argv[5]),parseTime(argv[6]),argc==8?std::stoll(argv[7]):60'000);
        std::cout << report.dump(2) << '\n';
        return report.at("mismatching").get<uint64_t>() ? 2 : 0;
    } catch(const std::exception& e) { std::cerr << "hmc2_diff: " << e.what() << '\n'; return 1; }
}
} // namespace sentinel::roller
