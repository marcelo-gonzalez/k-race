import sys
import numpy as np
from mpl_toolkits import mplot3d
import matplotlib.pyplot as plt

def add_plot(fig, dimensions, data, trigger_names, idx):
    if dimensions == 2:
        ax = fig.add_subplot(len(trigger_names), 1, idx+1, projection='3d')
        ax.scatter3D(data['offset_0'], data['offset_1'], data[trigger_names[idx]])
        ax.set_xlabel('offset 0')
        ax.set_ylabel('offset 1')
    else:
        ax = fig.add_subplot(len(trigger_names), 1, idx+1)
        ax.scatter(data['offset_0'], data[trigger_names[idx]])
        ax.set_xlabel('offset')
        # https://github.com/matplotlib/matplotlib/issues/6015
        extra = (max(data['offset_0']) - min(data['offset_0']))*.01
        ax.set_xlim(min(data['offset_0'])-extra, max(data['offset_0'])+extra)
    ax.set_title(trigger_names[idx])


def main():
    if len(sys.argv) < 2:
        print('pass me a file name')
        return

    fig = plt.figure()
    data = np.genfromtxt(sys.argv[1], delimiter=',', names=True)
    dimensions = 0
    triggers = []
    for i in range(0, len(data.dtype.names)):
        name = data.dtype.names[i]
        if name.startswith('offset'):
            dimensions += 1
            if (dimensions > 2):
                print('visualization with > 2 params not supported')
                return
        if name.endswith('triggers'):
            triggers.append(name)

    for i in range(len(triggers)):
        add_plot(fig, dimensions, data, triggers, i)

    plt.show()

if __name__ == '__main__':
    main()
