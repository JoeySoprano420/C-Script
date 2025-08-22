clang++ -std=c++17 cscriptc.cpp -o cscriptc \
  -DCS_EMBED_LLVM=1 -DCS_PGO_EMBED=1 \
  `llvm-config --cxxflags --ldflags --system-libs --libs all` \
  -lclangFrontend -lclangDriver -lclangCodeGen -lclangParse -lclangSema \
  -lclangSerialization -lclangAST -lclangLex -lclangBasic \
  -lclangToolingCore -lclangRewrite -lclangARCMigrate \
  -llldELF -llldCOFF -llldMachO -llldCommon
