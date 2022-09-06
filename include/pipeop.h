/* Copyright 2022 Simon Willcocks
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Inline routines to access OS_PipeOp pipes.
 */


enum { Create,
       WaitForSpace,  // Block task until N bytes may be written
       // WaitUntilEmpty,// Block task until all bytes have been consumed TODO?
       SpaceFilled,   // I've filled this many bytes
       PassingOver,   // Another task is going to take over filling this pipe
       UnreadData,    // Useful, in case data can be dropped or consolidated (e.g. mouse movements)
       NoMoreData,    // I'm done filling the pipe
       WaitForData,   // Block task until N bytes may be read (or WaitUntilEmpty, NoMoreData called)
       DataConsumed,  // I don't need the first N bytes that were written any more
       PassingOff,    // Another task is going to take over listening at this pipe
       NotListening   // I don't want any more data, thanks
       };

#ifndef __KERNEL_H
typedef struct {
  error_block *error;
  void *location;
  uint32_t available; 
} PipeSpace;

#define OS_PipeOp 0x200fa
#endif

static inline uint32_t PipeOp_CreateForTransfer( uint32_t max_block )
{
  register uint32_t code asm ( "r0" ) = Create;
  register uint32_t max_block_size asm ( "r2" ) = max_block;
  register uint32_t max_data asm ( "r3" ) = 0; // Unlimited
  register uint32_t allocated_mem asm ( "r4" ) = 0; // OS allocated

  register uint32_t pipe asm ( "r1" );

  asm volatile ( "svc %[swi]" 
             "\n  movvs r1, #0"
        : "=r" (pipe)
        : [swi] "i" (OS_PipeOp)
        , "r" (code)
        , "r" (max_block_size)
        , "r" (max_data)
        , "r" (allocated_mem)
        : "lr"
        );

  return pipe;
}

// This routine will return immediately if the requested space exceeds the capacity.
// This routine will return early if NotListening (-> space.location = 0) is called
// Data consumers, if they want to consume fixed-sized blocks at a time, should allocate at least one
// extra block of capacity
// This is complicated. I want the data to be aligned properly so that it can be sent to a device in
// pages, but if I have a few bytes in the pipe, then the writer wants to write the total capacity,
// it should be able to wait for that much space.
// Maybe the writer should be told how many (more) bytes the reader is waiting for, instead? But that's
// likely to be one byte.
// Allocate capacity + 1 block, report capacity each time.
static inline PipeSpace PipeOp_WaitForSpace( uint32_t write_pipe, uint32_t bytes )
{
  // IN
  register uint32_t code asm ( "r0" ) = WaitForSpace;
  register uint32_t pipe asm ( "r1" ) = write_pipe;
  register uint32_t amount asm ( "r2" ) = bytes;

  // OUT
  register error_block *error asm ( "r0" );
  register uint32_t available asm ( "r2" );
  register void *location asm ( "r3" );

  // gcc is working on allowing output and goto in inline assembler, but it's not there yet, afaik
  asm volatile (
        "svc %[swi]"
    "\n  movvc r0, #0"

        : "=r" (error)
        , "=r" (available)
        , "=r" (location)
        : [swi] "i" (OS_PipeOp)
        , "r" (code)
        , "r" (pipe)
        , "r" (amount)
        : "lr"
        );

  PipeSpace result = { .error = error, .location = location, .available = available };

  return result;
}

static inline PipeSpace PipeOp_SpaceFilled( uint32_t write_pipe, uint32_t bytes )
{
  // IN
  // In this case, bytes represents the number of bytes that the caller has written and
  // is making available to the reader.
  // The returned information is the same as from WaitForSpace and indicates the remaining
  // space after the filled bytes have been accepted. The virtual address of the remaining
  // data may not be the same as the address of the byte after the last accepted byte.
  register uint32_t code asm ( "r0" ) = SpaceFilled;
  register uint32_t pipe asm ( "r1" ) = write_pipe;
  register uint32_t amount asm ( "r2" ) = bytes;

  // OUT
  register error_block *error asm ( "r0" );
  register uint32_t available asm ( "r2" );
  register void *location asm ( "r3" );

  // gcc is working on allowing output and goto in inline assembler, but it's not there yet, afaik
  asm volatile (
        "svc %[swi]"
    "\n  movvc r0, #0"

        : "=r" (error)
        , "=r" (available)
        , "=r" (location)
        : [swi] "i" (OS_PipeOp)
        , "r" (code)
        , "r" (pipe)
        , "r" (amount)
        : "lr"
        );

  PipeSpace result = { .error = error, .location = location, .available = available };

  return result;
}

static inline PipeSpace PipeOp_WaitForData( uint32_t read_pipe, uint32_t bytes )
{
  // IN
  register uint32_t code asm ( "r0" ) = WaitForData;
  register uint32_t pipe asm ( "r1" ) = read_pipe;
  register uint32_t amount asm ( "r2" ) = bytes;

  // OUT
  register error_block *error asm ( "r0" );
  register uint32_t available asm ( "r2" );
  register void *location asm ( "r3" );

  // gcc is working on allowing output and goto in inline assembler, but it's not there yet, afaik
  asm volatile (
        "svc %[swi]"
    "\n  movvc r0, #0"

        : "=r" (error)
        , "=r" (available)
        , "=r" (location)
        : [swi] "i" (OS_PipeOp)
        , "r" (code)
        , "r" (pipe)
        , "r" (amount)
        : "lr"
        );

  PipeSpace result = { .error = error, .location = location, .available = available };

  return result;
}

static inline PipeSpace PipeOp_DataConsumed( uint32_t read_pipe, uint32_t bytes )
{
  // IN
  // In this case, the bytes are the number of bytes no longer of interest.
  // The returned information is the same as from WaitForData and indicates the remaining
  // data after the consumed bytes have been removed. The virtual address of the remaining
  // data may not be the same as the address of the byte after the last consumed byte.
  register uint32_t code asm ( "r0" ) = DataConsumed;
  register uint32_t pipe asm ( "r1" ) = read_pipe;
  register uint32_t amount asm ( "r2" ) = bytes;

  // OUT
  register error_block *error asm ( "r0" );
  register uint32_t available asm ( "r2" );
  register void *location asm ( "r3" );

  // gcc is working on allowing output and goto in inline assembler, but it's not there yet, afaik
  asm volatile (
        "svc %[swi]"
    "\n  movvc r0, #0"

        : "=r" (error)
        , "=r" (available)
        , "=r" (location)
        : [swi] "i" (OS_PipeOp)
        , "r" (code)
        , "r" (pipe)
        , "r" (amount)
        : "lr"
        );

  PipeSpace result = { .error = error, .location = location, .available = available };

  return result;
}

static inline error_block *PipeOp_PassingOff( uint32_t read_pipe, uint32_t new_receiver )
{
  // IN
  register uint32_t code asm ( "r0" ) = PassingOff;
  register uint32_t pipe asm ( "r1" ) = read_pipe;
  register uint32_t task asm ( "r2" ) = new_receiver;

  // OUT
  register error_block *error asm ( "r0" );

  // gcc is working on allowing output and goto in inline assembler, but it's not there yet, afaik
  asm volatile (
        "svc %[swi]"
    "\n  movvc r0, #0"

        : "=r" (error)
        : [swi] "i" (OS_PipeOp)
        , "r" (code)
        , "r" (pipe)
        , "r" (task)
        : "lr"
        );

  return error;
}

static inline error_block *PipeOp_PassingOver( uint32_t write_pipe, uint32_t new_sender )
{
  // IN
  register uint32_t code asm ( "r0" ) = PassingOver;
  register uint32_t pipe asm ( "r1" ) = write_pipe;
  register uint32_t task asm ( "r2" ) = new_sender;

  // OUT
  register error_block *error asm ( "r0" );

  // gcc is working on allowing output and goto in inline assembler, but it's not there yet, afaik
  asm volatile (
        "svc %[swi]"
    "\n  movvc r0, #0"

        : "=r" (error)
        : [swi] "i" (OS_PipeOp)
        , "r" (code)
        , "r" (pipe)
        , "r" (task)
        : "lr"
        );

  return error;
}

