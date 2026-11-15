#pragma once


namespace tz {

class DeclarationOrderContext {
public:
  unsigned takeSequenceNumber();
  unsigned getSequenceNumber() const;

private:
  unsigned sequenceNumber = 0;
};

}
