{ pkgs ? import (fetchTarball https://channels.nixos.org/nixos-20.03/nixexprs.tar.xz) {
    config.android_sdk.accept_license = true;
  }
}:

pkgs.mkShell {
  buildInputs = with pkgs; [
    python
    autoconf
    libtool
    automake
    pkgconfig
    zeromq
    czmq
    lua
    jansson
    lz4
    sqlite_3_31_1
    libsodium
    perl

    # The doc fails at make time, just ommiting this package enable flux-core to build.
    # asciidoc

    # (pythonPackages.buildPythonPackage rec {
    #   pname = "flux";
    #   version = "0.0.a";
    #   name = "${pname}-${version}";

    #   src = ./src/bindings/python;

    #   propagatedBuildInputs = with pythonPackages; [
    #     cffi
    #     six
    #     pyyaml
    #     jsonschema
    #   ];
    # })

    # Default hwloc from nixos is 2.xx and not compatible with flux-core
    (hwloc.overrideAttrs (attr:  rec {
      version = "1.11.1";
      pname = "hwloc";
      name = "${pname}-${version}";
      src = pkgs.fetchurl  {
          url = "https://download.open-mpi.org/release/hwloc/v1.11/hwloc-1.11.1.tar.gz";
          sha256 = "sha256:0qdy26kksqsyg985v6gja223g68j64jyymrxji06c0mng5yqf7xl";
        };
      }
    ))

    (python2.withPackages(ps: with ps; [
      cffi
      six
      pyyaml
      jsonschema
    ]))

  ];
}
