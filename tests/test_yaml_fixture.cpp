// Copyright (c) 2026 V. K. Safronov

#include "doctest.h"
#include "yaml_parser.h"

#include <cstdio>
#include <string>

// Synthetic-but-realistic iRacing session-info YAML string.
//
// This is NOT a real capture from a running iRacing session (none was
// available at the time this fixture was written); it is a hand-built
// raw string literal whose section names, nesting, and array-of-records
// shape mirror what src/iracing/iracing.cpp actually queries via
// parseYaml()/parseYamlInt()/parseYamlFloat()/parseYamlStr() (see the
// paths built with sprintf() around "WeekendInfo:", "DriverInfo:Drivers:
// CarIdx:{N}...:", and "SessionInfo:Sessions:SessionNum:{N}...:" in that
// file). A real dump-based fixture can replace/augment this later.
static const char* kSessionYaml = R"(
WeekendInfo:
  TrackName: monza full
  TrackID: 16
  SubSessionID: 123456789
  WeekendOptions:
    NumStarters: 2
    IsFixedSetup: 1
DriverInfo:
  DriverCarIdx: 0
  DriverCarFuelMaxLtr: 60.000
  DriverCarIdleRPM: 1000.000
  DriverCarRedLine: 9000.000
  Drivers:
    - CarIdx: 0
      UserName: Alice Anderson
      CarNumber: "12"
      CarNumberRaw: 12
      LicString: A 4.99
      LicColor: 0xfeffffff
      IRating: 3500
      CarIsPaceCar: 0
      IsSpectator: 0
      CurDriverIncidentCount: 2
      CarClassEstLapTime: 105.500
    - CarIdx: 1
      UserName: Bob Brown
      CarNumber: "7"
      CarNumberRaw: 7
      LicString: B 2.50
      LicColor: 0xfe40ff40
      IRating: 1800
      CarIsPaceCar: 0
      IsSpectator: 0
      CurDriverIncidentCount: 5
      CarClassEstLapTime: 108.250
SessionInfo:
  Sessions:
    - SessionNum: 0
      SessionName: Practice
      SessionType: Open Practice
      SessionTime: unlimited
      SessionLaps: unlimited
      ResultsPositions:
        - Position: 1
          CarIdx: 0
        - Position: 2
          CarIdx: 1
    - SessionNum: 1
      SessionName: Race
      SessionType: Race
      SessionTime: 1800.000000
      SessionLaps: unlimited
      ResultsPositions:
        - Position: 1
          CarIdx: 1
        - Position: 2
          CarIdx: 0
)";

TEST_CASE("fixture: WeekendInfo top-level scalar fields")
{
    const char* val = nullptr;
    int len = 0;

    REQUIRE(parseYaml(kSessionYaml, "WeekendInfo:TrackID:", &val, &len));
    CHECK(std::string(val, len) == "16");

    val = nullptr;
    len = 0;
    REQUIRE(parseYaml(kSessionYaml, "WeekendInfo:SubSessionID:", &val, &len));
    CHECK(std::string(val, len) == "123456789");
}

TEST_CASE("fixture: WeekendInfo nested option field (matches iracing.cpp path style)")
{
    const char* val = nullptr;
    int len = 0;

    REQUIRE(parseYaml(kSessionYaml, "WeekendInfo:WeekendOptions:IsFixedSetup:", &val, &len));
    CHECK(std::string(val, len) == "1");
}

TEST_CASE("fixture: DriverInfo scalar fields outside the Drivers array")
{
    const char* val = nullptr;
    int len = 0;

    REQUIRE(parseYaml(kSessionYaml, "DriverInfo:DriverCarIdx:", &val, &len));
    CHECK(std::string(val, len) == "0");

    val = nullptr;
    len = 0;
    REQUIRE(parseYaml(kSessionYaml, "DriverInfo:DriverCarFuelMaxLtr:", &val, &len));
    CHECK(std::string(val, len) == "60.000");
}

TEST_CASE("fixture: DriverInfo:Drivers:CarIdx:{N}UserName: for each car, exactly as iracing.cpp queries it")
{
    char path[128];
    const char* val = nullptr;
    int len = 0;

    std::snprintf(path, sizeof(path), "DriverInfo:Drivers:CarIdx:{%d}UserName:", 0);
    val = nullptr;
    len = 0;
    REQUIRE(parseYaml(kSessionYaml, path, &val, &len));
    CHECK(std::string(val, len) == "Alice Anderson");

    std::snprintf(path, sizeof(path), "DriverInfo:Drivers:CarIdx:{%d}UserName:", 1);
    val = nullptr;
    len = 0;
    REQUIRE(parseYaml(kSessionYaml, path, &val, &len));
    CHECK(std::string(val, len) == "Bob Brown");
}

TEST_CASE("fixture: DriverInfo:Drivers:CarIdx:{N}CarNumberRaw: and IRating:")
{
    char path[128];
    const char* val = nullptr;
    int len = 0;

    std::snprintf(path, sizeof(path), "DriverInfo:Drivers:CarIdx:{%d}CarNumberRaw:", 1);
    val = nullptr;
    len = 0;
    REQUIRE(parseYaml(kSessionYaml, path, &val, &len));
    CHECK(std::string(val, len) == "7");

    std::snprintf(path, sizeof(path), "DriverInfo:Drivers:CarIdx:{%d}IRating:", 0);
    val = nullptr;
    len = 0;
    REQUIRE(parseYaml(kSessionYaml, path, &val, &len));
    CHECK(std::string(val, len) == "3500");
}

TEST_CASE("fixture: DriverInfo:Drivers:CarIdx:{N}... for a CarIdx that does not exist returns false")
{
    char path[128];
    const char* val = nullptr;
    int len = 0;

    std::snprintf(path, sizeof(path), "DriverInfo:Drivers:CarIdx:{%d}UserName:", 7);
    CHECK_FALSE(parseYaml(kSessionYaml, path, &val, &len));
    CHECK(val == nullptr);
    CHECK(len == 0);
}

TEST_CASE("fixture: SessionInfo:Sessions:SessionNum:{N}SessionName: selects the right session")
{
    char path[128];
    const char* val = nullptr;
    int len = 0;

    std::snprintf(path, sizeof(path), "SessionInfo:Sessions:SessionNum:{%d}SessionName:", 0);
    val = nullptr;
    len = 0;
    REQUIRE(parseYaml(kSessionYaml, path, &val, &len));
    CHECK(std::string(val, len) == "Practice");

    std::snprintf(path, sizeof(path), "SessionInfo:Sessions:SessionNum:{%d}SessionName:", 1);
    val = nullptr;
    len = 0;
    REQUIRE(parseYaml(kSessionYaml, path, &val, &len));
    CHECK(std::string(val, len) == "Race");
}

TEST_CASE("fixture: SessionInfo:Sessions:SessionNum:{N}SessionTime: unlimited vs numeric")
{
    char path[128];
    const char* val = nullptr;
    int len = 0;

    std::snprintf(path, sizeof(path), "SessionInfo:Sessions:SessionNum:{%d}SessionTime:", 0);
    val = nullptr;
    len = 0;
    REQUIRE(parseYaml(kSessionYaml, path, &val, &len));
    CHECK(std::string(val, len) == "unlimited");

    std::snprintf(path, sizeof(path), "SessionInfo:Sessions:SessionNum:{%d}SessionTime:", 1);
    val = nullptr;
    len = 0;
    REQUIRE(parseYaml(kSessionYaml, path, &val, &len));
    CHECK(std::string(val, len) == "1800.000000");
}

TEST_CASE("fixture: nested double {N} filter, SessionNum:{N}ResultsPositions:Position:{N}CarIdx:")
{
    // Mirrors the iracing.cpp path style:
    //   "SessionInfo:Sessions:SessionNum:{%d}ResultsPositions:Position:{%d}CarIdx:"
    char path[160];
    const char* val = nullptr;
    int len = 0;

    // Session 0 (Practice): Position 1 -> CarIdx 0, Position 2 -> CarIdx 1.
    std::snprintf(path, sizeof(path),
        "SessionInfo:Sessions:SessionNum:{%d}ResultsPositions:Position:{%d}CarIdx:", 0, 1);
    val = nullptr;
    len = 0;
    REQUIRE(parseYaml(kSessionYaml, path, &val, &len));
    CHECK(std::string(val, len) == "0");

    // Session 1 (Race): Position 1 -> CarIdx 1, Position 2 -> CarIdx 0.
    std::snprintf(path, sizeof(path),
        "SessionInfo:Sessions:SessionNum:{%d}ResultsPositions:Position:{%d}CarIdx:", 1, 1);
    val = nullptr;
    len = 0;
    REQUIRE(parseYaml(kSessionYaml, path, &val, &len));
    CHECK(std::string(val, len) == "1");
}
