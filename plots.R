library(ggplot2)
library(sqldf)
library(Cairo)

## 100GB free RAM, Linux 5.11, swap off

d0=read.csv('all.csv')
d1=read.csv('rnd1.csv', col.names=c('time', 'readKB', 'c', 'd', 'e'), header=FALSE)
d2=read.csv('seq1.csv', col.names=c('time', 'readKB', 'c', 'd', 'e'), header=FALSE)
d3=read.csv('seq10.csv', col.names=c('time', 'readKB', 'c', 'd', 'e'), header=FALSE)
d=sqldf("
select * from
(select time, dev, seq, hint, threads,
        workGB / (time-lag(time) over (partition by dev, seq, hint, threads order by time)) workGBPerS,
        readGB / (time-lag(time) over (partition by dev, seq, hint, threads order by time)) readGBPerS,
        cast(tlb as float) / (time-lag(time) over (partition by dev, seq, hint, threads order by time)) tlbPerS,
        'mmap ' || (case hint when 0 then 'MADV_NORMAL' when 1 then 'MADV_RND' else 'MADV_SEQ' end) as mode
from d0
union all select round(time/1000.0)+0.5, '/blk/s2', 0, 0, 100, avg(readKB)/1024.0/1024, avg(readKB)/1024.0/1024, 0, 'fio O_DIRECT pread' from d1 group by round(time/1000.0)
union all select round(time/1000.0)+0.5, '/blk/s2', 1, 0, 20, avg(readKB)/1024.0/1024, avg(readKB)/1024.0/1024, 0, 'fio O_DIRECT libaio' from d2 group by round(time/1000.0)
union all select round(time/1000.0)+0.5, '/dev/md127', 1, 0, 20, avg(readKB)/1024.0/1024, avg(readKB)/1024.0/1024, 0, 'fio O_DIRECT libaio' from d3 group by round(time/1000.0)
)
where time > 2
")

CairoPDF('random_bw.pdf', 5.5, 2)
ggplot(sqldf("select * from d where dev = '/blk/s2' and seq = 0"), aes(time, workGBPerS*(1024*1024*1024)/4096/1e6, color=mode, shape=mode)) +
    geom_point() +
##    labs(title='random 4KB reads, 100 threads, 1 SSD') +
    scale_x_continuous('time [s]', limits = c(1,60)) +
    scale_y_continuous('reads per s', limits = c(0,1), breaks=c(0, 0.25, 0.5, 0.75, 1), labels=c('0', '250K', '500K', '750K', '1M')) +
    geom_vline(xintercept = 27, linetype = "longdash", color='darkgray') +
    geom_vline(xintercept = 31.5, linetype = "longdash", color='darkgray') +
    scale_color_brewer(palette="Set2") +
    theme_bw() +
    theme(legend.title = element_blank())
dev.off()

CairoPDF('random_tlb.pdf', 5.5, 2)
ggplot(sqldf("select * from d where dev = '/blk/s2' and seq = 0"), aes(time, tlbPerS/1e6, color=mode, shape=mode)) +
    geom_point() +
##    labs(title='random 4KB reads, 100 threads, 1 SSD') +
    scale_x_continuous('time [s]', limits = c(1,60)) +
    scale_y_continuous('TLB shootdowns per s', breaks=c(0,0.5,1,1.5,2), labels=c('0', '0.5M', '1.0M', '1.5M', '2.0M')) +
    geom_vline(xintercept = 27, linetype = "longdash", color='darkgray') +
    geom_vline(xintercept = 31.5, linetype = "longdash", color='darkgray') +
    scale_color_brewer(palette="Set2") +
    theme_bw() +
    theme(legend.title = element_blank())
dev.off()

CairoPDF('seq_1ssd.pdf', 5.5, 2)
ggplot(sqldf("select * from d where dev = '/blk/s2' and seq = 1"), aes(time, workGBPerS, color=mode, shape=mode)) +
    geom_point() +
##    labs(title='sequential, 20 threads, 1 SSD') +
    scale_x_continuous('time [s]', limits = c(1,60)) +
    scale_y_continuous('bandwidth [GB/s]') +
    scale_color_brewer(palette="Set2") +
    theme_bw() +
    theme(legend.title = element_blank())
dev.off()

CairoPDF('seq_10ssd.pdf', 5.5, 2)
ggplot(sqldf("select * from d where dev = '/dev/md127' and seq = 1"), aes(time, workGBPerS, color=mode, shape=mode)) +
    geom_point() +
##    labs(title='sequential, 20 threads, 10 SSDs') +
    scale_x_continuous('time [s]', limits = c(1,60)) +
    scale_y_continuous('bandwidth [GB/s]') +
    scale_color_brewer(palette="Set2") +
    theme_bw() +
    theme(legend.title = element_blank())
dev.off()
