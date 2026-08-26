#include "demo/network_space.h"
#include "emit.h"

int main() {
  demo::network::Storage net;
  demo::network::Stream stream{net, ae::tele::space::StreamMarker::kMain};
  demo::EmitPoll(stream);
  demo::EmitPktRx(stream, 64);
  auto blob = ae::tele::space::SerializeBlob(net);
  return static_cast<int>(blob.size() > 0);
}
