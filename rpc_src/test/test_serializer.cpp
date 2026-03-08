#include "serializer_mananger.h"

void TestJsonSerializer() {
  LOG_INFO("==============TEST JSON ===========");
  nlohmann::json data = {{"user_id", 123456}, {"user_name", "test_user"}};
  std::string json_serial =
      rpc::SerializerManager::Serialize(data, rpc::SerializerType::JSON);
  if (json_serial.empty()) {
    LOG_ERROR("json serialize failed");
  }
  { LOG_INFO("json serialize success: {}", json_serial); }

  nlohmann::json json_deserial;
  rpc::SerializerManager::Deserialize<nlohmann::json>(
      json_serial, json_deserial, rpc::SerializerType::JSON);
  if (json_deserial.empty()) {
    LOG_ERROR("json deserialize failed");
  }
  { LOG_INFO("json deserialize success: {}", json_deserial.dump()); }
}

void TestProtoSerializer() {
  LOG_INFO("==============TEST PROTO ===========");
  minirpc::HelloRequest req;
  req.set_name("test_user");
  std::string proto_serial =
      rpc::SerializerManager::Serialize(req, rpc::SerializerType::PROTOBUF);
  if (proto_serial.empty()) {
    LOG_ERROR("proto serialize failed");
  }
  { LOG_INFO("proto serialize success: {}", proto_serial); }
  minirpc::HelloRequest proto_deserial;
  rpc::SerializerManager::Deserialize<minirpc::HelloRequest>(
      proto_serial, proto_deserial, rpc::SerializerType::PROTOBUF);
  if (proto_deserial.name().empty()) {
    LOG_ERROR("proto deserialize failed");
  }
  { LOG_INFO("proto deserialize success: {}", proto_deserial.name()); }
}

int main() {
  TestJsonSerializer();
  TestProtoSerializer();
  return 0;
}