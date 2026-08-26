#include "demo/application_space.h"
#include "demo/network_space.h"
#include "emit.h"

int main() {
  demo::network::Storage net;
  demo::application::Storage app;
  demo::network::Stream ns{net, ae::tele::space::StreamMarker::kMain};
  demo::application::Stream as{app, ae::tele::space::StreamMarker::kMain};
  demo::EmitPoll(ns);
  demo::EmitFrame(as);
  return static_cast<int>(net.TotalLogBytes() + app.TotalLogBytes());
}
