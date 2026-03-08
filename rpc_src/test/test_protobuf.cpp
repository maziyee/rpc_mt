#include <iostream>

#include "log_manager.h"
#include "rpc_protobuf.h"

void TestRpcRequest() {
  rpc::RpcRequest request;
  request.SetServiceName("test");
  request.SetMethodName("test");
  request.SetPayload("test");
  std::string serialized_request;
  request.Serializer(serialized_request);
  rpc::RpcRequest deserialized_request;
  deserialized_request.Deserializer(serialized_request);
  assert(request.GetServiceName() == deserialized_request.GetServiceName());
  assert(request.GetMethodName() == deserialized_request.GetMethodName());
  assert(request.GetPayload() == deserialized_request.GetPayload());
  LOG_INFO("TestRpcRequest success");
}

void TestRpcResponse() {
  rpc::RpcResponse response;
  response.SetResultData("test");
  response.SetErrorCode(0);
  response.SetErrorMessage("test");
  std::string serialized_response;
  response.Serializer(serialized_response);
  rpc::RpcResponse deserialized_response;
  deserialized_response.Deserializer(serialized_response);
  assert(response.GetResultData() == deserialized_response.GetResultData());
  assert(response.GetErrorCode() == deserialized_response.GetErrorCode());
  assert(response.GetErrorMessage() == deserialized_response.GetErrorMessage());
  LOG_INFO("TestRpcResponse success");
}
int main() {
  TestRpcRequest();
  TestRpcResponse();
  return 0;
}