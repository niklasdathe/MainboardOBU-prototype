FROM python:3.13-alpine

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1

WORKDIR /app

RUN addgroup -S bridge && adduser -S -D -H -u 10001 -G bridge bridge

COPY requirements.txt ./
RUN pip install --no-cache-dir -r requirements.txt

COPY bridge.py protocol.py ./

USER bridge

ENTRYPOINT ["python", "/app/bridge.py"]
