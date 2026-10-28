#include "asset-builder.h"
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#include <unistd.h>
namespace fs = std::filesystem;
void Wait(AssetBuilder& builder) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (builder.Snapshot().running && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(!builder.Snapshot().running);
}
int main() {
    auto dir = fs::temp_directory_path() / ("kyty asset tests ' " + std::to_string(getpid()));
    fs::create_directories(dir);
    AssetBuilder builder;
    auto script = dir / "mock build.sh";
    std::ofstream(script) << "printf '[FSR412] Recovering\\n'; printf 'problem on stderr\\n' >&2; printf '%s\\n' \"$1\"; sleep .15; exit 4\n";
    assert(builder.Start({"bash",script.string(),"DLL path with ' spaces"},dir/"build.log"));
    assert(!builder.Start({"false"},dir/"other.log"));
    bool live = false;
    for (int i=0;i<100;i++) {
        auto s=builder.Snapshot();
        if (s.running && s.stage=="Recovering" && s.output.find("problem on stderr")!=std::string::npos) {live=true;break;}
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(live); Wait(builder);
    assert(builder.Snapshot().exit_code==4);
    assert(builder.Snapshot().output.find("DLL path with ' spaces")!=std::string::npos);
    assert(fs::file_size(dir/"build.log")>0);
    assert(builder.Start({"bash","-c","echo '[FSR411] Installing'; exit 0"},dir/"success.log"));
    Wait(builder); assert(builder.Snapshot().exit_code==0);
    assert(!AssetBuilder::Installed(dir/"missing"));
    const auto assets=dir/"assets";
    for (const char* set : {"t1080_m0", "t1080_m1", "t2160_m0", "t2160_m1"}) {
        fs::create_directories(assets/set);
        std::ofstream(assets/set/"initializer.bin",std::ios::binary) << std::string(131072,'\0');
        for(const char* pass : {"spd", "prepass", "pass0_post", "pass1", "pass1_post", "pass2", "pass2_post",
            "pass3", "pass3_post", "pass4", "pass4_post", "pass5", "pass5_post", "pass6", "pass6_post", "pass7",
            "pass7_post", "pass8", "pass8_post", "pass9", "pass9_post", "pass10", "pass10_post", "pass11",
            "pass11_post", "pass12", "pass12_post", "postpass", "rcas"}) {
            std::ofstream file(assets/set/(std::string(pass)+".spv"),std::ios::binary);
            const unsigned char magic[]={3,2,35,7};file.write(reinterpret_cast<const char*>(magic),4);
        }
    }
    assert(!AssetBuilder::Installed(assets));
    std::ofstream(assets/"ASSET_VERSION") << "FSR411\n";
    assert(!AssetBuilder::Installed(assets));
    std::ofstream(assets/"ASSET_VERSION") << "FSR412_VULKAN_INT8_4.1.2.4555\n";
    assert(AssetBuilder::Installed(assets));
    std::ofstream(assets/"t2160_m1"/"initializer.bin",std::ios::trunc) << "broken";
    assert(!AssetBuilder::Installed(assets));
    assert(builder.Start({"bash","-c","sleep 30 & wait"},dir/"cancel.log"));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    builder.Cancel(); Wait(builder);
    assert(builder.Snapshot().stage=="Cancelled");
    assert(builder.Start({"/no/such/executable"},dir/"spawn.log")); Wait(builder);
    assert(builder.Snapshot().exit_code==-1);
    { AssetBuilder close; assert(close.Start({"bash","-c","sleep 30 & wait"},dir/"close.log")); }
    fs::remove_all(dir);
    std::cout << "Asset-builder live output, stderr, failure/retry, quoting, cancellation and shutdown passed\n";
}
