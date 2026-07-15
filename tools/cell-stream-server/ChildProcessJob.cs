using System;
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace CellStreamServer
{
   // a Windows Job Object that kills every process assigned to it once the last handle to the job closes.
   // ffmpeg children go in it, so on the paths that never run our own Kill() - the server killed in Task
   // Manager, or a hard crash - the OS reaps ffmpeg along with us, instead of leaving it holding the screen
   // capture (which then blocks the next launch, since Windows hands the capture to one process at a time).
   internal static class ChildProcessJob
   {
      private static readonly IntPtr Job = CreateKillOnCloseJob();

      // best-effort: pre-Win8 a process can only belong to one job, so this can fail if a launcher already
      // put the server in one. that just falls back to the old behaviour, so a failure is not worth surfacing.
      public static void Assign(Process process)
      {
         if (Job == IntPtr.Zero) return;
         try { AssignProcessToJobObject(Job, process.Handle); } catch { }
      }

      private static IntPtr CreateKillOnCloseJob()
      {
         IntPtr handle = CreateJobObject(IntPtr.Zero, null);
         if (handle == IntPtr.Zero) return IntPtr.Zero;

         var info = new JobObjectExtendedLimitInformation();
         info.BasicLimitInformation.LimitFlags = JobObjectLimitKillOnJobClose;
         int size = Marshal.SizeOf(typeof(JobObjectExtendedLimitInformation));
         IntPtr infoPointer = Marshal.AllocHGlobal(size);
         try
         {
            Marshal.StructureToPtr(info, infoPointer, false);
            SetInformationJobObject(handle, ExtendedLimitInformationClass, infoPointer, (uint)size);
         }
         finally { Marshal.FreeHGlobal(infoPointer); }
         return handle;
      }

      private const int JobObjectLimitKillOnJobClose = 0x2000;
      private const int ExtendedLimitInformationClass = 9;

      [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
      private static extern IntPtr CreateJobObject(IntPtr security, string name);

      [DllImport("kernel32.dll")]
      private static extern bool SetInformationJobObject(IntPtr job, int infoClass, IntPtr info, uint infoLength);

      [DllImport("kernel32.dll")]
      private static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);

      [StructLayout(LayoutKind.Sequential)]
      private struct JobObjectBasicLimitInformation
      {
         public long PerProcessUserTimeLimit, PerJobUserTimeLimit;
         public int LimitFlags;
         public UIntPtr MinimumWorkingSetSize, MaximumWorkingSetSize;
         public int ActiveProcessLimit;
         public UIntPtr Affinity;
         public int PriorityClass, SchedulingClass;
      }

      [StructLayout(LayoutKind.Sequential)]
      private struct IoCounters
      {
         public ulong ReadOperationCount, WriteOperationCount, OtherOperationCount;
         public ulong ReadTransferCount, WriteTransferCount, OtherTransferCount;
      }

      [StructLayout(LayoutKind.Sequential)]
      private struct JobObjectExtendedLimitInformation
      {
         public JobObjectBasicLimitInformation BasicLimitInformation;
         public IoCounters IoInfo;
         public UIntPtr ProcessMemoryLimit, JobMemoryLimit, PeakProcessMemoryUsed, PeakJobMemoryUsed;
      }
   }
}
