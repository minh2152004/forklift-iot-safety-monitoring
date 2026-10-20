FROM python:3.10-slim

# Install system dependencies (needed for OpenCV)
RUN apt-get update && apt-get install -y \
    libgl1 \
    libglib2.0-0 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy files
COPY . .

# Install Python dependencies
RUN pip install --no-cache-dir \
    flask \
    numpy \
    opencv-python-headless \
    ultralytics \
    paho-mqtt

# Expose Flask port
EXPOSE 5001

# Run server
CMD ["python", "server.py"]