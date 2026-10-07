#!/bin/bash
set -e

minikube status >/dev/null 2>&1 || minikube start
docker build -t subnet-calculator:1.0 .
minikube image rm subnet-calculator:1.0 >/dev/null 2>&1 || true
minikube image load subnet-calculator:1.0
helm upgrade --install subnet-calc ./helm/subnet-calculator
kubectl rollout status deployment/subnet-calc --timeout=60s

echo "Conéctese con: telnet $(minikube ip) 30966"
