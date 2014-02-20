/**
 * range.hpp
 *
 *  Created on: Feb 18, 2014
 *      Author: tpan
 */

#ifndef RANGE_HPP_
#define RANGE_HPP_

#include <cassert>
#include <iostream>

namespace bliss
{
  namespace iterator
  {
    /**
     * Range specified with offset, length, and overlap.  specific for 1D.
     */
    template<typename T>
    struct range {

        T block_start;   // starting position of range aligned to block
        T start;   // offset from the beginning of block, if range is block aligned.
        T end;   // length of range, starting from offset_in_block.
        T step;   // stride is the distance between each successive elements
        T overlap;  // amount of overlap at each end

        range(T const& _start,
              T const& _end,
              T const& _overlap = 0,
              T const& _step = 1)
          : block_start(_start),
            start(_start),
            end(_end),
            step(_step),
            overlap(_overlap)
        {}

        range(range<T> const &other)
          : block_start(other.block_start),
            start(other.start),
            end(other.end),
            step(other.step),
            overlap(other.overlap)
        {}

        range()
        : block_start(0),
          start(0),
          end(0),
          step(1),
          overlap(0)
        {}

        range<T>& operator=(range<T> const & other)
        {
          block_start = other.block_start;
          start = other.start;
          end = other.end;
          step = other.step;
          overlap = other.overlap;
          return *this;
        }

        // TODO: when needed: comparators
        // TODO: when needed: +/-
        // TODO: when needed: set operations
        // TODO: stream printing operator.

        /**
         *  block partitioning
         */
        static range<T> block_partition(T   const& total,
                                        int const& np,
                                        int const& pid,
                                        T   const& _overlap = 0,
                                        T   const& _step = 1)
        {
          assert(total > 0);
          assert(_overlap >= 0);
          assert(_step > 0);
          assert(np > 0);
          assert(pid >= 0 && pid < np);

          range<T> output(0, total, _overlap, _step);

          if (np == 1)
            return output;


          T div = total / static_cast<T>(np);
          T rem = total % static_cast<T>(np);
          if (static_cast<T>(pid) < rem)
          {
            output.start = static_cast<T>(pid) * (div + 1);
            output.end = output.start + (div + 1) + _overlap;
          }
          else
          {
            output.start = static_cast<T>(pid) * div + rem;
            output.end = output.start + div + _overlap;
          }

          assert(output.start < total);
          if (output.end > total)
            output.end = total;

          output.block_start = output.start;
          return output;
        }

        /**
         * align the range to page boundaries.
         */
        range<T> align_to_page(T const &page_size) const
        {
          range<T> output(*this);

          // change start to align by page size.  extend range end
          output.block_start = (this->start / page_size) * page_size;
          // leave end as is.

          return output;
        }

        bool is_page_aligned(T const &page_size) const
        {
          return this->block_start % page_size == 0;
        }

        /**
         * non-overlapping block partitions.
         *
         * takes into account the block size (e.g. page size) to force alignment
         *
         * uses overlap.  kernel handles bringing whole page in for the last (not full) page.
         *
         * result is the returned ranges are block aligned, ranges are mutually exclusive.
         */
        // deprecated
        //static range<T> block_partition_page_aligned(const T1& total, const T1& overlap, const T1& blocksize, const T2& np, const T2& pid)
        //{
        //  assert(total > 0);
        //  assert(blocksize > 0);
        //  assert(overlap >= 0);
        //  assert(np > 0);
        //  assert(pid >= 0 && pid < np);
        //
        //  RangeType<T1> output;
        //  output.overlap = overlap;
        //
        //  if (np == 1)
        //  {
        //    output.offset = 0;
        //    output.length = total;
        //    return output;
        //  }
        //
        //  // spread the number of blocks first.
        //  T1 nblock = total / blocksize;
        //
        //  T1 div = nblock / static_cast<T1>(np);
        //  T1 rem = nblock % static_cast<T1>(np);
        //  if (static_cast<T1>(pid) < rem)
        //  {
        //    output.offset = static_cast<T1>(pid) * (div + 1) * blocksize;
        //    output.length = (div + 1) * blocksize + overlap;
        //  }
        //  else
        //  {
        //    output.offset = (static_cast<T1>(pid) * div + rem) * blocksize;
        //    output.length = div * blocksize + overlap;
        //  }
        //
        //  assert(output.offset < total);
        //  if ((output.offset + output.length) >= total)
        //    output.length = total - output.offset;
        //
        //  return output;
        //}


    };

    template<typename T>
    std::ostream& operator<<(std::ostream& ost, const range<T>& r) {
      ost << "range: block@" << r.block_start << " [" << r.start << ":" << r.step << ":" << r.end << ") overlap " << r.overlap;
      return ost;
    }


  } /* namespace functional */
} /* namespace bliss */
#endif /* RANGE_HPP_ */
