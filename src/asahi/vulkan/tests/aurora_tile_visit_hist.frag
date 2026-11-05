#version 450
// No colour output, a discard: hdr_histogram's shape, counted by a query.
void main() { if (fract(gl_FragCoord.x / 64.0) < 0.3) discard; }
