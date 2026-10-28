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
    std::ofstream(script) << "printf '[FSR411] Capturing\\n'; printf 'problem on stderr\\n' >&2; printf '%s\\n' \"$1\"; sleep .15; exit 4\n";
    assert(builder.Start({"bash",script.string(),"DLL path with ' spaces"},dir/"build.log"));
    assert(!builder.Start({"false"},dir/"other.log"));
    bool live = false;
    for (int i=0;i<100;i++) {
        auto s=builder.Snapshot();
        if (s.running && s.stage=="Capturing" && s.output.find("problem on stderr")!=std::string::npos) {live=true;break;}
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(live); Wait(builder);
    assert(builder.Snapshot().exit_code==4);
    assert(builder.Snapshot().output.find("DLL path with ' spaces")!=std::string::npos);
    assert(fs::file_size(dir/"build.log")>0);
    assert(builder.Start({"bash","-c","echo '[FSR411] Installing'; exit 0"},dir/"success.log"));
    Wait(builder); assert(builder.Snapshot().exit_code==0);
    assert(!AssetBuilder::Installed(dir/"missing"));
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
