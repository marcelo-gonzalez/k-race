#!/usr/bin/env python3

import argparse
import os
import struct
import sys
import pandas as pd
from mpl_toolkits import mplot3d
import matplotlib.pyplot as plt

def add_plot(fig, data):
    if data.shape[1] > 4:
        raise ValueError('Plotting with more than 3 k-race threads not suppoorted')

    if data.shape[1] == 4:
        ax = fig.add_subplot(projection='3d')
        ax.scatter3D(data['offset_0'], data['offset_1'], data['triggers'])
        ax.set_xlabel('offset 0')
        ax.set_ylabel('offset 1')
    else:
        ax = fig.add_subplot()
        ax.scatter(data['offset_0'], data['triggers'])
        ax.set_xlabel('offset')
        # https://github.com/matplotlib/matplotlib/issues/6015
        extra = (max(data['offset_0']) - min(data['offset_0']))*.01
        ax.set_xlim(min(data['offset_0'])-extra, max(data['offset_0'])+extra)
    ax.set_title('triggers')

def single_datapoint_len(num_params):
    return num_params*8+4+4


def read_lines(f, lines, data_fmt, num_params):
    data = []
    while True:
        if lines > 0 and len(data) >= lines:
            break

        x = f.read(single_datapoint_len(num_params))
        if len(x) < single_datapoint_len(num_params):
            break
        data.append(struct.unpack(data_fmt, x))
    return data

def read_k_race_file(filename, lines=0):
    with open(filename, 'rb') as f:
        magic = f.read(len('k_race_data'))
        if magic != b'k_race_data':
            raise ValueError('%s does not appear to be a k-race output file' % filename)

        nump = f.read(4)
        num_params = struct.unpack('<I', nump)[0]

        # little endian
        data_fmt = '<'
        # signed 64 bits for each param
        for i in range(num_params):
            data_fmt += 'q'
        # unsigned 32 bits for counts and triggers
        data_fmt += 'II'

        if lines >= 0:
            data = read_lines(f, lines, data_fmt, num_params)
            index_start = 0
        else:
            lines = -lines
            start = f.seek(0, os.SEEK_CUR)
            end = f.seek(0, os.SEEK_END)

            p = end - lines * single_datapoint_len(num_params)
            if p < start:
                p = start
            f.seek(p, os.SEEK_SET)
            data = read_lines(f, lines, data_fmt, num_params)
            index_start = (p-start)/single_datapoint_len(num_params)

        columns = []
        for i in range(num_params):
            columns.append('offset_%d' % i)
        columns += ['counts', 'triggers']
        ret = pd.DataFrame(data, columns=columns,
                           index=pd.RangeIndex(index_start, index_start + len(data)))
        ret['triggers'] /= ret['counts']
        ret.fillna(value=0, inplace=True)
        return ret

def main():
    parser = argparse.ArgumentParser(description='display k-race output')
    subparsers = parser.add_subparsers(dest='cmd')

    plot_parser = subparsers.add_parser('plot',
                                        help='display a plot of the data (only available for data output by 3 or fewer k-race threads)')
    cat_parser = subparsers.add_parser('cat',
                                       help='dump human-readable data to stdout')

    head_parser = subparsers.add_parser('head', help='display the first N lines of the output')
    head_parser.add_argument('-n', dest='n', type=int, default=10, help='the number of lines to print')

    tail_parser = subparsers.add_parser('tail', help='display the last N lines of the output')
    tail_parser.add_argument('-n', dest='n', type=int, default=10, help='the number of lines to print')

    parser.add_argument('file', help='k-race output file')
    args = parser.parse_args()

    if (args.cmd == 'head' or args.cmd == 'tail') and args.n <= 0:
        print('the argument given with -n must be positive', file=sys.stderr)
        sys.exit(1)

    if args.cmd == 'head':
        data = read_k_race_file(args.file, args.n)
    elif args.cmd == 'tail':
        data = read_k_race_file(args.file, -args.n)
    else:
        data = read_k_race_file(args.file)

    if args.cmd == 'plot':
        fig = plt.figure()
        add_plot(fig, data)
        plt.show()
    else:
        with pd.option_context('display.max_rows', None):
            print(data)

if __name__ == '__main__':
    main()
