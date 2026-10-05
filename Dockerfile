FROM python:3.10-slim

WORKDIR /app

# Install system dependencies for pygame, opencv
RUN apt-get update && apt-get install -y \
    libsdl2-2.0-0 \
    libsdl2-image-2.0-0 \
    libsdl2-mixer-2.0-0 \
    libsdl2-ttf-2.0-0 \
    libgl1 \
    libglib2.0-0 \
    && rm -rf /var/lib/apt/lists/*

# Install python dependencies
RUN pip install --no-cache-dir pygame websockets pyyaml opencv-python-headless flask

# Environment variables for headless pygame
ENV SDL_VIDEODRIVER=dummy
ENV SDL_AUDIODRIVER=dummy

COPY . /app/

CMD ["python", "-u", "unified_server.py"]
