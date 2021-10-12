# suspend-blocker

The suspend-blocker tool parses the kernel log and will explain the reasons that suspend was woken up or which wakelocks blocked suspend from working. It is useful on Android devices that use wakelocks.

## suspend-blocker command line options

* -b list blocking wakelock names and count
* -h show help
* -H histogram of times between suspend and suspend duration
* -r list causes of resume
* -v verbose information 

## Example Output:

```
suspend-blocker -r -H  < /var/log/kern.log 
stdin:
Resume wakeup causes:
  IRQ 177, nfc_irq                  681  47.82%
  IRQ 162, bcmsdh_sdmmc             678  47.61%
  IRQ 163, gpio_keys                 29   2.04%
  I/O pad: CONTROL_PADCONF_WAK       20   1.40%
  IRQ 39, TWL6030-PIH                13   0.91%
  wakeup I/O pad: CONTROL_WKUP        1   0.07%

Period of time between each successful suspend:
     0.000 -    0.124 seconds        15   2.02%
     0.125 -    0.249 seconds         0   0.00%
     0.250 -    0.499 seconds         0   0.00%
     0.500 -    0.999 seconds         3   0.40%
     1.000 -    1.999 seconds       459  61.86%
     2.000 -    3.999 seconds       111  14.96%
     4.000 -    7.999 seconds        49   6.60%
     8.000 -   15.999 seconds        25   3.37%
    16.000 -   31.999 seconds        13   1.75%
    32.000 -   63.999 seconds         9   1.21%
    64.000 -  127.999 seconds        36   4.85%
   128.000 -  255.999 seconds        11   1.48%
   256.000 -  511.999 seconds         6   0.81%
   512.000 -          seconds         2   0.27%

Duration of successful suspends:
     0.000 -    0.124 seconds        14   1.88%
     0.125 -    0.249 seconds        15   2.02%
     0.250 -    0.499 seconds        29   3.90%
     0.500 -    0.999 seconds        27   3.63%
     1.000 -    1.999 seconds        41   5.52%
     2.000 -    3.999 seconds        56   7.54%
     4.000 -    7.999 seconds        68   9.15%
     8.000 -   15.999 seconds        78  10.50%
    16.000 -   31.999 seconds       132  17.77%
    32.000 -   63.999 seconds       190  25.57%
    64.000 -  127.999 seconds        90  12.11%
   128.000 -  255.999 seconds         3   0.40%

Stats:
  74 suspends aborted (9.06%).
  743 suspends succeeded (90.94%).
  33.200499 seconds average suspend duration (min 0.001000, max 250.768000).
```
