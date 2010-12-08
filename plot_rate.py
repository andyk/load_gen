#!/usr/bin/env python
import sys
import math
import fileinput
import matplotlib
matplotlib.use("PDF")
import matplotlib.pyplot as pyplot

window = 1

def key(val):
  return int(val*(1.0/window)) * window

hist = {}
for line in fileinput.input():
  parts = line.split(" ")
  val = float(parts[0])
  k = key(val)

  if k in hist:
    hist[k] += 1.0/window
  else:
    hist[k] = 1.0/window

items = hist.items()
items.sort()
k, v = zip(*items)
pyplot.plot(k, v)
pyplot.savefig('plot.pdf')
print "Created plot.pdf"
