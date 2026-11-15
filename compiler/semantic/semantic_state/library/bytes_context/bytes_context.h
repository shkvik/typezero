#pragma once


namespace tz {

class BytesContext {
public:
  void enterDecoderParse();
  void leaveDecoderParse();


  bool isInsideDecoderParse() const;

private:
  int decoderParseDepth = 0;
};

}
