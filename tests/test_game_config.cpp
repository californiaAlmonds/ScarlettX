#include "sx_check.h"
#include <iostream>

#include "scarlettx/loader/game_config.h"

int main() {
    const std::string scarlett_xml = R"(
<?xml version="1.0" encoding="utf-8"?>
<Game configVersion="1">
  <Identity Name="TestStudio.ScarlettGame" Publisher="CN=Test" Version="1.2.3.4" />
  <TitleId>74B7FFFC</TitleId>
  <ShellVisuals DefaultDisplayName="Test Scarlett Title" />
  <ExecutableList>
    <Executable Name="Game.exe" Id="Game" TargetDeviceFamilyForXboxOne="XboxGen9" />
  </ExecutableList>
</Game>
)";

    scarlettx::GameConfig cfg;
    auto st = scarlettx::ParseGameConfigXml(scarlett_xml, cfg);
    SX_REQUIRE(st.ok());
    SX_REQUIRE(cfg.IsNativeScarlett());
    SX_REQUIRE(cfg.title_id == "74B7FFFC");
    SX_REQUIRE(cfg.identity_name == "TestStudio.ScarlettGame");
    SX_REQUIRE(cfg.PrimaryExecutable() != nullptr);
    SX_REQUIRE(cfg.PrimaryExecutable()->name == "Game.exe");

    const std::string formatted_xml = R"(
<Game configVersion = '1'>
  <Identity Name = 'TestStudio.FormattedGame' />
  <ShellVisuals DefaultDisplayName = 'Arcade > Racing &amp; &quot;Rally&quot; &apos;98 &lt;Demo&gt; &amp;lt;' />
  <ExecutableList>
    <Executable OverrideDisplayName="Alternate Title" Alias='Name="Wrong.exe"'
                Name
                = 'Formatted.exe' TargetDeviceFamily = "Scarlett" />
  </ExecutableList>
</Game>
)";

    scarlettx::GameConfig formatted_cfg;
    st = scarlettx::ParseGameConfigXml(formatted_xml, formatted_cfg);
    SX_REQUIRE(st.ok());
    SX_REQUIRE(formatted_cfg.config_version == "1");
    SX_REQUIRE(formatted_cfg.identity_name == "TestStudio.FormattedGame");
    SX_REQUIRE(formatted_cfg.display_name == "Arcade > Racing & \"Rally\" '98 <Demo> &lt;");
    SX_REQUIRE(formatted_cfg.IsNativeScarlett());
    SX_REQUIRE(formatted_cfg.PrimaryExecutable() != nullptr);
    SX_REQUIRE(formatted_cfg.PrimaryExecutable()->name == "Formatted.exe");

    const std::string gen9_aware_xml = R"(
<Game configVersion="0">
  <Identity Name="TestStudio.OldGame" Publisher="CN=Test" Version="1.0.0.0" />
  <ExecutableList>
    <Executable Name="OldGame.exe" TargetDeviceFamilyForXboxOne="XboxOneGen9Aware" />
  </ExecutableList>
</Game>
)";
    scarlettx::GameConfig old_cfg;
    st = scarlettx::ParseGameConfigXml(gen9_aware_xml, old_cfg);
    SX_REQUIRE(st.ok());
    SX_REQUIRE(!old_cfg.IsNativeScarlett());
    SX_REQUIRE(old_cfg.console_target == scarlettx::ConsoleTarget::XboxOneGen9Aware);

    scarlettx::GameConfig bad_cfg;
    st = scarlettx::ParseGameConfigXml("<NotAGame/>", bad_cfg);
    SX_REQUIRE(!st.ok());

    std::cout << "[test_game_config] PASSED\n";
    return 0;
}
