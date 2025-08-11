#!/bin/bash
git fetch origin
for branch in $(git branch -r | grep "origin/" | grep -v "->"); do
  b=${branch#origin/}
  git branch --track $b $branch 2>/dev/null || echo "$b already exists"
done
