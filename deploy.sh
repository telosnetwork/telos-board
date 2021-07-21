#! /bin/bash

contract="telos.board"

#account
account=$1

#network
if [[ "$2" == "mainnet" ]]; then 
    url=http://telos.caleos.io
    network="Telos Mainnet"
elif [[ "$2" == "testnet" ]]; then
    url=https://testnet.telos.caleos.io
    network="Telos Testnet"
elif [[ "$2" == "local" ]]; then
    url=http://127.0.0.1:8888
    network="Local"
else
    echo "need network"
    exit 0
fi

echo ">>> Deploying $contract contract to $account on $network..."

# eosio v1.8.0
cleos -u $url set contract $account ./build/$contract/ $contract.wasm $contract.abi -p $account