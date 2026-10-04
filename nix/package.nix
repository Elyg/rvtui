{
  lib,
  stdenv,
  cmake,
  ninja,
  pkg-config,
  ftxui,
  spdlog,
  openexr,
  stb,
  zlib,
  libdeflate,
  cli11,
  gtest,
  version ? "dev",
}:

stdenv.mkDerivation {
  pname = "rvtui";
  inherit version;

  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [
      ../CMakeLists.txt
      ../cmake
      ../src
      ../tests
    ];
  };

  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
  ];
  buildInputs = [
    ftxui
    spdlog
    openexr
    stb
    zlib
    libdeflate
    cli11
  ];
  checkInputs = [ gtest ];

  cmakeFlags = [
    (lib.cmakeFeature "RVTUI_VERSION" version)
    (lib.cmakeBool "RVTUI_BUILD_TESTS" true)
  ];

  # Tests write temporary EXRs; fine in the sandbox.
  doCheck = true;

  meta = {
    description = "Terminal image browser/viewer for OpenEXR (layers, AOVs, sequences), PNG and JPEG";
    mainProgram = "rvtui";
    platforms = lib.platforms.unix;
  };
}
