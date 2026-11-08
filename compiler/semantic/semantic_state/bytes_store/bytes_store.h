#pragma once

namespace tz {

class BytesStore {
public:
  void enterDecoderParse();
  void leaveDecoderParse();
  bool isInsideDecoderParse() const;

private:
  int decoderParseDepth = 0;
};

}
