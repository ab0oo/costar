#!/bin/bash
# Deploy solar_proxy.py to the VPS alongside the existing RSS proxy.
# Usage: ./deploy.sh [user@host]  (default: john@vps.gorkos.net)

set -e
TARGET="${1:-john@vps.gorkos.net}"

echo "==> Deploying solar_proxy.py to $TARGET"
ssh "$TARGET" "sudo mkdir -p /opt/costar-proxy && sudo chown \$USER /opt/costar-proxy"
scp solar_proxy.py "$TARGET:/opt/costar-proxy/"
scp solar_proxy.service "$TARGET:/tmp/"
ssh "$TARGET" "sudo mv /tmp/solar_proxy.service /etc/systemd/system/ && \
               sudo systemctl daemon-reload && \
               sudo systemctl enable solar_proxy && \
               sudo systemctl restart solar_proxy && \
               sleep 2 && \
               sudo systemctl status solar_proxy --no-pager"

echo "==> Done. Test with: curl http://vps.gorkos.net:8086/solar | python3 -m json.tool"
