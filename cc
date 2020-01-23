#!/bin/bash
cd "$(dirname "$0")"
rm user1/*.user2
rm user1/*.user3
rm user2/*.user1
rm user2/*.user3
rm user3/*.user1
rm user3/*.user2
