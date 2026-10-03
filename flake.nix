{
  description = "Host-side checks for the ESCargot Express firmware.";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-26.05";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    { nixpkgs, flake-utils, ... }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = import nixpkgs { inherit system; };
      in
      {
        # `nix develop`, and what CI runs tests/check.sh inside.
        #
        # The point is that "it passed locally" says something about CI. The
        # analysis tools are version-sensitive: a cppcheck or clang-tidy from
        # whatever the runner image ships can report findings another version
        # does not, and in the sibling firmware repository exactly that had
        # the Checks job red on two findings newer versions do not make. Here
        # they currently agree, which is luck rather than a property.
        #
        # Not the ESP-IDF builds. Those stay on the espressif container in the
        # workflow: the toolchain is large, versioned by Espressif, and the
        # thing being tested is that their SDK builds our tree.
        devShells.default = pkgs.mkShell {
          packages = with pkgs; [
            # main/script/test: the host tests, under asan and ubsan.
            gcc
            gnumake

            # tools/luapack.py and tools/script_ext_coverage.py.
            python3

            # The analysis stages.
            cppcheck
            clang-tools

            # libFuzzer, for the container parser. clang-tools brings
            # clang-tidy but not the compiler the fuzzer needs.
            clang

            # main/lispBM/repl links against both, and its sources include
            # their headers -- a plain `nix shell` puts the libraries on the
            # path without the include flags, which is why the documented
            # build instruction uses a shell with them as inputs.
            readline
            libpng
          ];
        };
      }
    );
}
