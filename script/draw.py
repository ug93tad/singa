import numpy as np
import matplotlib.pyplot as plt
import matplotlib
import sys
import glob
import argparse
import matplotlib.cm as cm

#matplotlib.rcParams.update({'font.size': 19})

def make_N_colors(cmap_name, N):
  cmap = cm.get_cmap(cmap_name, N)
  return cmap(np.arange(N))



if __name__=="__main__":
  parser=argparse.ArgumentParser(description='plot lines')
  parser.add_argument('prefix', help='file prefix')
  parser.add_argument('x', type=int, help='column for x-axis')
  parser.add_argument('y', type=int, help='column for y-axis')
  args=parser.parse_args();

  files=glob.glob(args.prefix);
  print files
  color=make_N_colors('gist_rainbow', len(files))

  fig=plt.figure()
  ax=fig.add_subplot(111)
  handles=[]
  for idx, f in enumerate(files):
    dat=np.loadtxt(f)
    x=dat[:, args.x]+1
    y=dat[:, args.y]
    handles.append(ax.plot(x,y,color=color[idx], label=f)[0])
  ax.set_xscale('log')
  plt.legend(handles=handles)
  plt.show()
