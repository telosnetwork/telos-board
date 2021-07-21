#! /bin/bash

#contract
contract="telos.board"

echo ">>> Testing $contract contract..."

#copy build to tests folder
# cp build/todo/todo.wasm tests/contracts/todo/
# cp build/todo/todo.abi tests/contracts/todo/

#start nodeos
eoslime nodeos start

#run test suite
mocha tests/todoTests.js

#stop nodeos
eoslime nodeos stop
