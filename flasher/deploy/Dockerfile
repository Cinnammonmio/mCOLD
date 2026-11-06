# The line's web app (mflash web). Build from the repository root:
#   docker build -f flasher/deploy/Dockerfile -t mcold-line .
FROM python:3.12-slim

WORKDIR /app
COPY flasher/requirements.txt /app/flasher/requirements.txt
RUN pip install --no-cache-dir -r /app/flasher/requirements.txt

# the code, the fonts the page uses, and the partition table builds are checked against
COPY flasher /app/flasher
COPY mcold-spec/fonts /app/mcold-spec/fonts
COPY firmware/partitions.csv /app/firmware/partitions.csv

# everything that changes lives in /data: settings, records, the firmware library
ENV MFLASH_SITE=/data/site.ini \
    MFLASH_DB=/data/mflash.db \
    MFLASH_LIBRARY=/data/library \
    MFLASH_WEB_STATE=/data/web.json \
    MFLASH_DEBUG_DIR=/data/debug \
    PYTHONUNBUFFERED=1
VOLUME /data
WORKDIR /app/flasher
EXPOSE 8800
# behind the https proxy (Caddyfile); Web Serial needs https
CMD ["python", "-m", "mflash.web", "--host", "0.0.0.0", "--port", "8800"]
