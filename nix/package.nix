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
  opencolorio,
  gtest,
  version ? "dev",
}:

let
  # The static (musl) build: no GL apps or python, which have no static libs.
  ocio =
    if stdenv.hostPlatform.isStatic then
      (opencolorio.override {
        pythonBindings = false;
        buildApps = false;
        glew = null;
        libglut = null;
      }).overrideAttrs
        { doCheck = false; }
    else
      opencolorio;
in
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
    ocio
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
