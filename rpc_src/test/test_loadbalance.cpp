#include <string>
#include <vector>

#include "load_balance.h"

int main() {
  rpc::LoadBanlance::InitLoadBanlance("hash");
  std::vector<std::string> servers = {
      "192.168.1.100:8001", "192.168.1.101:8001", "192.168.1.102:8001"};
  std::vector<std::string> clients = {"127.0.0.1:8000", "192.168.1.100:8080",
                                      "127.0.0.1:8000"};
  for (auto client : clients) {
    auto server = rpc::LoadBanlance::SelectServers(servers, client);
    std::cout << "client : " << client << " , server:" << server << "\n";
    std::cout.flush();
  }
  return 0;
}