#!/bin/bash

id=${1}

sed -i '/#define NODE_ID_VALUE/ c\#define NODE_ID_VALUE '"$id"'' src/main.c

type=${2}

make download-$type