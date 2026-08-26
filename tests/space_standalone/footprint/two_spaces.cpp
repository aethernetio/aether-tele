#include "demo/application_space.h"
#include "demo/network_space.h"
#include "emit.h"

int main() {
  demo::network::Storage net;
  demo::application::Storage app;
  demo::network::Stream ns{net, ae::tele::space::StreamMarker::kMain};
  demo::application::Stream as{app, ae::tele::space::StreamMarker::kMain};
  demo::EmitPoll(ns);
  demo::EmitRequest(ns, 10);
  demo::EmitFrame(as);
  demo::EmitWork(as, 4);
  auto nb = ae::tele::space::SerializeBlob(net);
  auto ab = ae::tele::space::SerializeBlob(app);
  return static_cast<int>(nb.size() + ab.size() > 0);
}
