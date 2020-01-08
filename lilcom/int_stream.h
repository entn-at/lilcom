#ifndef LILCOM_INT_STREAM_H_
#define LILCOM_INT_STREAM_H_ 1

#include <stdint.h>
#include <sys/types.h>
#include <vector>
#include "int_math_utils.h"  /* for num_bits() */
#include "bit_stream.h"
#include <iostream>


/**
   This header contains declarations for UintStream and IntStream objects
   whose job it is to pack (respectively) unsigned and signed integers into
   bit-streams.  It uses an algorithm that is especially efficient when
   there are correlations between the magnitudes of successive values
   written (i.e. if small values tend to follow small values, and large values
   tend to follow large values).
*/


/**
   class UintStream (with the help of class BitStream) is responsible for coding
   32-bit integers into a sequence of bytes.

   See also class ReverseUintStream and class IntStream.
 */
class UintStream {
 public:


  /*  Constructor */
  UintStream(): most_recent_num_bits_(0),
                started_(false),
                flushed_(false),
                num_pending_zeros_(0) { }


  /*
    Write the bits.  The lower-order `num_bits_in` of `bits_in` will
    be written to the stream.
      @param [in] value  The number to write.

   Note: you must call Write() AT LEAST ONCE; this class does not
   support writing an empty stream.
   */
  inline void Write(uint32_t value) {
    buffer_.push_back(value);
    if (buffer_.size() >= 64) {
      FlushSome(32);
    }
  }

  /*
    Flush the stream.  Should be called once, after you are done
    calling Write().  [Note: Write() must be called at least once
    before calling Flush().]
   */
  inline void Flush() {
    assert(!flushed_);
    assert(!buffer_.empty());  /* check that data has been written. */
    flushed_ = true;
    FlushSome(buffer_.size());
    if (num_pending_zeros_) {
      //std::cout << "In Flush():";
      FlushPendingZeros();
    }
    bit_stream_.Flush();
  }

  inline void FlushPendingZeros() {
    assert(num_pending_zeros_ >= 1);
    /*
      This writes the code that dictates how many zeros are in a run of zeros.
      (this is, of course, after we already got into a situation where
      the num_bits is zero, which would have been a code '1, then 0' starting
      from num_bits=1.)

      The code is:
           1 -> 1 zero, then a 1 [or end of stream]
           01x -> 2+x zeros, then a 1 [or end of stream].  (could be 2 or 3 zeros)
           001xx -> 4+xx zeros, then a 1 [or end of stream].  (could be 4,5,6,7 zeros)
           0001xxx -> 8+xxx zeros, then a 1 [or end of stream]. ...
              and so on.
       Note:
     */

    /* -1 below because we don't need to write the most significant bit. */
    int num_bits_written = int_math::num_bits(num_pending_zeros_) - 1;

    /* think of the following as writing `num_bits_written` zeros, then
       a 1. */
    bit_stream_.Write(num_bits_written + 1, 1 << num_bits_written);

    /* Write `num_pending_zeros_`, except for the top bit.  We couldn't just
       write num_bits_written zeros, then num_pending_zeros_ in
       num_bits_written+1 bits, because they'd be in the wrong order; we need
       the `1` to be first so we can identify where the zeros end.
     */
    bit_stream_.Write(num_bits_written,
                      num_pending_zeros_ & ((1 << num_bits_written) - 1));
    //std::cout << "In FlushPendingZeros, nz = " << num_pending_zeros_ << "\n";
    num_pending_zeros_ = 0;
  }

  /* Gets the code that was written.  You should not call thes
     before having called Flush(). */
  const std::vector<int8_t> &Code() const {
    assert(flushed_);
    return bit_stream_.Code();
  }

 private:

  /* buffer_ contains pending values that we have not yet encoded. */
  std::vector<uint32_t> buffer_;

  /* most_recent_num_bits_ is 0 if we have not yet called FlushSome();
     otherwise is is the num-bits of the most recent int that was
     written to the packer_ object (i.e. the one previous to the
     int in buffer_[0]).
  */
  int most_recent_num_bits_;

  BitStream bit_stream_;

  /**
     This function outputs to the `num-bits` array the number of bits
     for each corresponding element of buffer_, increased as necessary
     to ensure that successive elements differ by no more than 1
     (and the zeroth element is no less than most_recent_num_bits_ - 1).

       `num_bits_out` must have size equal to buffer_.size().
  */
  inline void ComputeNumBits(std::vector<int> *num_bits_out) const {
    int prev_num_bits = most_recent_num_bits_;

    std::vector<uint32_t>::const_iterator buffer_iter = buffer_.begin(),
        buffer_end = buffer_.end();
    std::vector<int>::iterator num_bits_iter = num_bits_out->begin();
    /* Simplified version of the code below without end effects treated
       right is:
       for (i = 0 ... size-1):
        num_bits_[i] = max(num_bits[i-1] - 1, num_bits(buffer[i]))
    */
    for (; buffer_iter != buffer_end; ++buffer_iter, ++num_bits_iter) {
      *num_bits_iter = (prev_num_bits = int_math::int_math_max(
          int_math::num_bits(*buffer_iter),
          prev_num_bits - 1));
    }

    std::vector<int>::reverse_iterator num_bits_riter = num_bits_out->rbegin(),
        num_bits_rend = num_bits_out->rend();
    int next_num_bits = 0;
    /* Simplified version of the code below without end effects being
       treated correctly is:
       for (i = size-1, size-2, ... 0):
         num_bits[i] = max(num_bits[i+1] - 1, num_bits[i]);
    */
    for (; num_bits_riter != num_bits_rend; ++num_bits_riter) {
      int this_num_bits = *num_bits_riter;
      next_num_bits = *num_bits_riter = int_math::int_math_max(this_num_bits,
                                                               next_num_bits - 1);
    }
    /*
    size_t size = buffer_.size();
    std::cout << "size = " << size;
    for (int i = 0; i < size; i++) {
      std::cout << "  n=" << buffer_[i] << ", nbits=" << (*num_bits_out)[i];
    }
    std::cout << std::endl;
    */
  }

  /**
     This function takes care of writing the code for one integer ('i') to the
     stream.  Note: the order we write the codes is: each time, we write
     the 1 or 2 bits that determine the *next* sample's num_bits, and then
     we write the bits for *this* sample.  The reason is that we need to
     know the next-sample's num_bits to know whether the most significant
     bit of this sample is to be written.


     (This code does not handle runs of zeros; in fact,
     that is not implemented yet.  We just have a reserved space in the code.)

        @param [in] prev_num_bits  The num_bits of the previous integer...
                             see ComputeNumBits() for what this means.
        @param [in] cur_num_bits  The num_bits of the current 'integer'...
                             see ComputeNumBits() for what num_bits means.
                             It will be greater than or equal to num_bits(i).
                             (search int_math_utils.h for num_bits).
        @param [in] next_num_bits  The num_bits for the next integer
                             in the stream.
        @param [in] i        The integer that we are encoding.
                             (Must satisfy num_bits(i) <= cur_num_bits).
   */
  inline void WriteCode(int prev_num_bits,
                        int cur_num_bits,
                        int next_num_bits,
                        uint32_t i) {
    if (cur_num_bits == 0) {
      num_pending_zeros_++;
      /* Nothing is actually written in this case, until FlushPendingZeros()
         is called.  Since there are 0 bits in this integer, we don't have
         to write any to encode the actual value.
      */
      return;
    } else {
      if (num_pending_zeros_)
        FlushPendingZeros();
    }


    /*std::cout << "Called WriteCode: " << prev_num_bits << ", "
      << cur_num_bits << ", " << next_num_bits << ", " << i << std::endl;*/
    assert(int_math::num_bits(i) <= cur_num_bits);
    /* We write things out of order.. we encode the num_bits of the *next* sample
       before the actual value of this sample.  Ths is needed because of how the
       top_bit_redundant condition works (we'll need the num_bits of the next
       sample in order to encode the current sample's value). */
    int delta_num_bits = next_num_bits - cur_num_bits;

    if (delta_num_bits == 1) {
      /* Write 3 as a 2-bit number.  Think of this as writing a 1-bit, then a
       * 1-bit. */
      bit_stream_.Write(2, 3);
    } else if (delta_num_bits == -1) {
      /* Write 1 as a 2-bit number.  Think of this as writing a 1-bit, then a
       * 0-bit.  (Those written first become the lower order bits). */
      bit_stream_.Write(2, 1);
    } else {
      assert(delta_num_bits == 0);
      /* Write 0 as a 1-bit number.  We allocate half the probability space
         to the num_bits staying the same, then a quarter each to going up
         or down. */
      bit_stream_.Write(1, 0);
    }
    /* if top_bit_redundant is true then cur_num_bits will be exactly
       equal to num_bits(i), so we don't need to write out the highest-order
       bit of i (we know it's set). */
    bool top_bit_redundant = (prev_num_bits <= cur_num_bits &&
                              next_num_bits <= cur_num_bits &&
                              cur_num_bits > 0);

    if (!top_bit_redundant) {
      bit_stream_.Write(cur_num_bits, i);
    } else {
      /* top_bit_redundant is true, so we don't write the top bit.
         We have to zero it out, since BitStream::Write() requires that
         only the bits to write be nonzero.  The caret operator (^)
         takes care of that.
      */
      assert((i & (1 << (cur_num_bits - 1))) != 0);
      bit_stream_.Write(cur_num_bits - 1, i ^ (1 << (cur_num_bits - 1)));
    }
  }

  /**
     Flushes out up to `num_to_flush` ints from `buffer_` (or exactly
     `num_to_flush` ints if it equals buffer_.size().  This is called
     by Write(), and also by Flush().
  */
  inline void FlushSome(uint32_t num_to_flush) {
    size_t size = buffer_.size();
    assert(num_to_flush <= size);
    if (size == 0)
      return;  /* ? */

    /* num_bits contains an upper bound on the number of bits in each element of
       buffer_, from 0 to 32.  We choose the smallest sequence of num_bits
       such that num_bits[i] >= num_bits(buffer_[i]) and successive elements
       of num_bits differ by no more than 1.

       So basically we compute the num_bits() of each element of the buffer,
       then increase the values as necessary to ensure that the
       absolute difference between successive values is no greater than 1.
     */
    std::vector<int> num_bits(size);
    ComputeNumBits(&num_bits);
    if (num_to_flush == size) {
      /* end of stream.  we need to modify for end effects... */
      num_bits.push_back(num_bits.back());
    }

    if (!started_) {
      int first_num_bits = num_bits[0];
      /* write the num_bits of the first sample using 5 bits
         for all num_bits < 31; and if the num_bits is 31 or 32
         then specify it with one more bit.
      */
      bit_stream_.Write(5, (first_num_bits == 32 ? 31 : first_num_bits));
      if (first_num_bits >= 31)
        bit_stream_.Write(1, first_num_bits - 31);
      started_ = true;
      /* treat it as if there had been a value at time t=-1 with
         `first_num_bits` bits.. this only affects the top_bit_redundant
         condition. */
      most_recent_num_bits_ = first_num_bits;
    }
    int prev_num_bits = most_recent_num_bits_,
        cur_num_bits = num_bits[0];

    std::vector<uint32_t>::const_iterator iter = buffer_.begin();
    for (size_t i = 0; i < num_to_flush; i++,++iter) {
      int next_num_bits = num_bits[i+1];
      /* we're writing the element at buffer_[i] to the bit stream, and
         also encoding the exponent of the element following it. */
      WriteCode(prev_num_bits,
                cur_num_bits,
                next_num_bits,
                *iter);
      prev_num_bits = cur_num_bits;
      cur_num_bits = next_num_bits;
    }
    most_recent_num_bits_ = num_bits[num_to_flush - 1];
    buffer_.erase(buffer_.begin(), buffer_.begin() + num_to_flush);
  }
  /* started_ is true if we have called FlushSome() at least once. */
  bool started_;

  /* flushed_ will be set when Flush() has been called by the user. */
  bool flushed_;

  /* num_pending_zeros_ has to do with run-length encoding of sequences
     of 0's. It's the number of zeros in the sequence that we
     need to write. */
  uint32_t num_pending_zeros_;

};


class ReverseUintStream {
 public:
  /*
     Constructor
         @param [in] code  First byte of the code to be read.
         @param [in] code_memory_end  Pointer to one past the
                          end of the memory region allocated for
                          `code`.  This is only to prevent
                          segmentation faults in case a
                          corrupted or invalid stream is
                          attempted to be decoded; in most cases,
                          we'll never reach there.
                          MUST be greater than `code`.
   */
  ReverseUintStream(const int8_t *code,
                    const int8_t *code_memory_end):
      bit_reader_(code, code_memory_end),
      zero_runlength_(-1) {
    assert(code_memory_end > code);
    uint32_t num_bits;
    bool ans = bit_reader_.Read(5, &num_bits);
    assert(ans);
    if (num_bits >= 31) {
      /* we need an extra bit to distinguish between 31 and 32 (the
         initial num_bits can be anywhere from 0 to 32). */
      uint32_t extra_bit;
      ans = bit_reader_.Read(1, &extra_bit);
      assert(ans);
      num_bits += extra_bit;
    }
    prev_num_bits_ = num_bits;
    cur_num_bits_ = num_bits;
  }

  /*
    Read in an integer that was encoded by class UintStream.
        @param [out] int_out  The integer that was encoded will
                        be written to here.
        @return   Returns true on success, false on failure
                  (failure can result from a corrupted or too-short
                  input stream.)
  */
  inline bool Read(uint32_t *int_out) {
    int prev_num_bits = prev_num_bits_,
        cur_num_bits = cur_num_bits_,
        next_num_bits;
    uint32_t bit1, bit2;

    /* The following big if/else statement sets next_num_bits. */

    if (cur_num_bits != 0) {  /* the normal case, cur_num_bits > 0 */
      if (!bit_reader_.Read(1, &bit1))
        return false;  /* truncated code? */
      if (bit1 == 0) {
        next_num_bits = cur_num_bits;
      } else {
        if (!bit_reader_.Read(1, &bit2))
          return false;  /* truncated code? */
        if (bit2) {
          next_num_bits = cur_num_bits + 1;
          if (next_num_bits > 32)
            return false;  /* corrupted code? */
        } else {
          next_num_bits = cur_num_bits - 1;
          if (next_num_bits < 0)
            return false;  /* corrupted code? */
        }
      }
    } else {
      /* cur_num_bits == 0; this is treated specially. */
      if (zero_runlength_ >= 0) {
        /* We have already read the code for the sequence of zeros,
           and set zero_runlength_; we are in the middle of consuming
           it. */
        if (zero_runlength_ == 0) {
          /* we came to the end of the run and now need to output next_num_bits = 1. */
          next_num_bits = 1;
        } else {
          next_num_bits = 0;
        }
        /* note: zero_runlength_ may go to -1 below, which is by design. */
        zero_runlength_--;
      } else {
        /* Assume we have just arrived at the situation where
           cur_num_bits == 0, i.e. the previous num_bits was 1 and
           we encountered the code "1, then 0".
           We have to interpret the following bits to figure out the
           run-length, i.e. the number of zeros.  The code once we reach
           the cur_num_bits=0 situation is as follows:

              1 -> 1 zero, then a 1 [or end of stream]
              01x -> 2+x zeros, then a 1 [or end of stream].  (could be 2 or 3 zeros)
              001xx -> 4+xx zeros, then a 1 [or end of stream].  (could be 4,5,6,7 zeros)
              0001xxx -> 8+xxx zeros, then a 1 [or end of stream]. ...
                and so on.

           Please note that in the explanation above, the binary digits are
           displayed in the order in which they are written, which is from least
           to most significant, rather than the way a human would normally write
           them.
        */
        int num_zeros_read = 0;
        uint32_t bit;
        while (1) {
          if (!bit_reader_.Read(1, &bit) || num_zeros_read > 31)
            return false;  /* truncated or corrupted code? */
          if (bit == 0)
            num_zeros_read++;
          else  /* the bit was 1. */
            break;
        }
        uint32_t x;
        bit_reader_.Read(num_zeros_read, &x);
        //std::cout << "Num-zeros-read=" << x << "\n";

        int num_zeros_in_run = (1 << num_zeros_read) + x;
        /* minus 2 because we already have cur_num_bits == 0,
           so that's the first zero; then we are about to set
           next_num_bits, so that's potentially the second zero.
           If num_zeros_in_run is 1 (the lowest possible value),
           then zero_runlength_ will be -1, which is intended.
        */
        zero_runlength_ = num_zeros_in_run - 2;
        if (num_zeros_in_run == 1) {
          /* the next num_bits is 1. */
          next_num_bits = 1;
        } else {
          next_num_bits = 0;
        }
      }
    }

    bool top_bit_redundant = (prev_num_bits <= cur_num_bits &&
                              next_num_bits <= cur_num_bits &&
                              cur_num_bits > 0);
    if (top_bit_redundant) {
      if (!bit_reader_.Read(cur_num_bits - 1, int_out))
        return false;
      /* we know the top bit is 1, so don't need to read it. */
      *int_out |= (1 << (cur_num_bits - 1));
    } else {
      if (!bit_reader_.Read(cur_num_bits, int_out))
        return false;
    }
    prev_num_bits_ = cur_num_bits;
    cur_num_bits_ = next_num_bits;
    return true;
  }



  /*
     Returns a pointer to one past the end of the last byte read;
     may be needed, for instance, if we know another bit stream is
     directly after this one.
   */
  const int8_t *NextCode() const { return bit_reader_.NextCode(); }

 private:
  ReverseBitStream bit_reader_;

  /* prev_num_bits_ is the num-bits of the most recently read integer
     from the stream. */
  int prev_num_bits_;
  /* cur_num_bits_ is the num-bits of the integer that we are about to
     read from the stream. */
  int cur_num_bits_;

  /*  */
  std::vector<int> pending_num_bits_;
  /* the number of 0-bits we've just seen in the stream starting from
     where the num-bits first became zero (however, this gets reset
     if we have just encoded a run of zeros
  */
  int zero_runlength_;
};

/*
  This encodes signed integers (will be effective when the
  values are close to zero).
 */
class IntStream: public UintStream {
 public:
  IntStream() { }

  inline void Write(int32_t value) {
    UintStream::Write(value >= 0 ? 2 * value : -(2 * value) - 1);
  }
};

/*
  This class is for decoding data encoded by class IntStream.
 */
class ReverseIntStream: public ReverseUintStream {
 public:
  ReverseIntStream(const int8_t *code,
                   const int8_t *code_memory_end):
      ReverseUintStream(code, code_memory_end) { }

  inline bool Read(int32_t *value) {
    uint32_t i;
    if (!ReverseUintStream::Read(&i)) {
      return false;
    } else {
      *value = (i % 2 == 0 ? (int32_t)(i/2) : -(int32_t)(i/2) - 1);
      return true;
    }
  }
};


/* class Truncation operates on a stream of ints and tells us how much
   to truncate them, based on a user-specified configuration.  (The idea
   is that this operates on an audio stream, and tells us how many of
   the least significant bits to remove depending on the current
   volume of the residual).

   The number of bits to truncate is always determined by previously
   decoded samples, so it does not have to be transmitted (once the
   configuration values of this class are known).
*/
class Truncation {

  /*
    Construtor.


     @param [in] num_significant_bits
           This is the number of significant bits we'll use FOR QUIET SOUNDS.
           (We may use more for loud sounds, see alpha).  Specifically: we will
           only start truncating once the rms value of the input exceeds
           num_significant_bits.  Must be > 2.
     @param [in] alpha

           Alpha, which must be in the range [3..64], determines how many more
           significant bits we will allocate as the sound gets louder.
           The rule is as follows: suppose `energy` is the average energy
           per sample of the signal, and `num_bits(energy)` is the number of
           bits in it (see int_math::num_bits())... we will
           compute the number of bits to truncate (`bits_to_truncate`) as
           follows:

              # note: extra_bits is the number of extra bits in the energy above
              # what it would be if the rms value of the signal was equal to
              # `num_significant_bits`.  If we remove extra_bits/2 bits from the
              # signal, we'll get about `num_significant_bits` significant bits
              # remaining.

              extra_bits = num_bits(energy) - (2*num_significant_bits)

              bits_to_truncate = extra_bits/2 - extra_bits/alpha

           If alpha is large (e.g. 64) then extra_bits/alpha will always be
           zero and we'll have about `num_significant_bits`
           bits in the signal.  If alpha is very small (e.g. 4), the
           number of significant bits will rise rapidly as the signal
           gets louder.  You will probably want to tune alpha
           (and block_size, which is the size of the block from which
           the average energy is computed) based on perceptual
           considerations.
   */
  Truncator(int num_significant_bits,
            int alpha,
            int block_size):
      num_significant_bits_(num_significant_bits),
      block_size_(block_size),  /* e.g. 32 */
      count_(0),
      truncated_bits_(0) { }

  /* This function will return the current number of bits to truncate (while
     compressing) or the number of bits that have been truncated (while
     decompressing). */
  int GetNumTruncatedBits() const {
    return num_truncated_bits_;
  }

  /**
     This function is to be called in sequence for each integer in the stream
     (after GetNumTruncatedBits(), because you'll need that to get the input).

     It updates the state of this object, which will eventually have the
     effect of updating the number of bits to truncate.

       @param [in] i   The element of the stream AFTER TRUNCATION, i.e.
                    the value that would be written to the stream.
                    It would have to be shifted left by num_truncated_bits_
                    to get the actual signal value.
                    CAUTION: although i_truncated is of type int32_t you
                    are not allowed to use the full range of int32_t:
                    specifically, i_truncated * block_size_ must still
                    be representable as int32_t.  (This is no problem
                    for our applications, as block_size will be small,
                    e.g. 32 bits, and the elements of the stream will
                    have either max 17 bits (for residuals of a 16-bit
                    stream) or 25 bits (for residuals of a 24-bit
                    stream, which is not implemented yet).
   */
  void Step(int32_t i_truncated) {
    count_++;
    sumsq_ += i_truncated * (int64_t)i_truncated;

    if (count_ == block_size_)
      Update();
  }

 private:
  void Update() {

  }


  int num_truncated_bits_;
  int block_size_;

  /* count_ is the number of */
  int count_;
  int64_t sumsq_;

};

class TruncatedIntStream: public IntStream {
 public:
  /* e.g. num_significant_bits = 5, 6, 7, 8 depending on desired quality;
     block_size might be 32 for instance. */
  TruncatedIntStream(int num_significant_bits,
                     int block_size):
      num_significant_bits_(num_significant_bits),
      block_size_(block_size),  /* e.g. 32 */
      count_(0),
      truncated_bits_(0) { }



};



#endif /* LILCOM_INT_STREAM_H_ */
