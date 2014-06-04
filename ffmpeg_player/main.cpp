
//
// Disclamer:
// ----------
//
// This code will work only if you selected window, graphics and audio.
//
// Note that the "Run Script" build phase will copy the required frameworks
// or dylibs to your application bundle so you can execute it on any OS X
// computer.
//
// Your resource files (images, sounds, fonts, ...) are also copied to your
// application bundle. To get the path to these resource, use the helper
// method resourcePath() from ResourcePath.hpp
//

#include <SFML/Audio.hpp>
#include <SFML/Graphics.hpp>

// Here is a small helper for you ! Have a look.
#include "ResourcePath.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libavutil/channel_layout.h>
}

#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>

std::mutex  g_mut;
std::condition_variable g_newPktCondition;
std::deque<AVPacket*> g_audioPkts;

class MovieSound : public sf::SoundStream
{
public:
    MovieSound(AVFormatContext* ctx, int index);
    virtual ~MovieSound();
    
private:
    
    virtual bool onGetData(Chunk& data);
    virtual void onSeek(sf::Time timeOffset){}
    
    bool decodePacket(AVPacket* packet, AVFrame* outputFrame, bool& gotFrame);
    void initResampler();
    void resampleFrame(AVFrame* frame, uint8_t*& outSamples, int& outNbSamples, int& outSamplesLength);
    
    AVFormatContext* m_formatCtx;
    AVCodecContext* m_codecCtx;
    int m_audioStreamIndex;
    
    unsigned m_sampleRate;
    sf::Int16* m_samplesBuffer;
    AVFrame* m_audioFrame;
    
    SwrContext* m_swrCtx;
    int m_dstNbSamples;
    int m_maxDstNbSamples;
    int m_dstNbChannels;
    int m_dstLinesize;
    uint8_t** m_dstData;
};

MovieSound::MovieSound(AVFormatContext* ctx, int index)
: m_formatCtx(ctx)
, m_audioStreamIndex(index)
, m_codecCtx(ctx->streams[index]->codec)
{
    m_audioFrame = av_frame_alloc();
    
    m_sampleRate = m_codecCtx->sample_rate;
    m_samplesBuffer = (sf::Int16*)av_malloc(sizeof(sf::Int16) * av_get_channel_layout_nb_channels(AV_CH_LAYOUT_STEREO) * m_sampleRate * 2);
    
    initialize(av_get_channel_layout_nb_channels(AV_CH_LAYOUT_STEREO), m_sampleRate);
    
    initResampler();
    
}

MovieSound::~MovieSound()
{
}

void MovieSound::initResampler()
{
    int err = 0;
    m_swrCtx = swr_alloc();
    
    if(m_codecCtx->channel_layout == 0)
    {
        m_codecCtx->channel_layout = av_get_default_channel_layout(m_codecCtx->channels);
    }
    
    /* set options */
    av_opt_set_int(m_swrCtx, "in_channel_layout",    m_codecCtx->channel_layout, 0);
    av_opt_set_int(m_swrCtx, "in_sample_rate",       m_codecCtx->sample_rate, 0);
    av_opt_set_sample_fmt(m_swrCtx, "in_sample_fmt", m_codecCtx->sample_fmt, 0);
    av_opt_set_int(m_swrCtx, "out_channel_layout",    AV_CH_LAYOUT_STEREO, 0);
    av_opt_set_int(m_swrCtx, "out_sample_rate",       m_codecCtx->sample_rate, 0);
    av_opt_set_sample_fmt(m_swrCtx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
    
    err = swr_init(m_swrCtx);
    
    m_maxDstNbSamples = m_dstNbSamples = 1024;
    
    m_dstNbChannels = av_get_channel_layout_nb_channels(AV_CH_LAYOUT_STEREO);
    err = av_samples_alloc_array_and_samples(&m_dstData, &m_dstLinesize, m_dstNbChannels, m_dstNbSamples, AV_SAMPLE_FMT_S16, 0);
}

bool MovieSound::decodePacket(AVPacket* packet, AVFrame* outputFrame, bool& gotFrame)
{
    bool needsMoreDecoding = false;
    int igotFrame = 0;
    
    int decodedLength = avcodec_decode_audio4(m_codecCtx, outputFrame, &igotFrame, packet);
    gotFrame = (igotFrame != 0);
    
    if(decodedLength < packet->size)
    {
        needsMoreDecoding = true;
        packet->data += decodedLength;
        packet->size -= decodedLength;
    }
    
    return needsMoreDecoding;
}

void MovieSound::resampleFrame(AVFrame *frame, uint8_t *&outSamples, int &outNbSamples, int &outSamplesLength)
{
    int err = 0;
    int src_rate = frame->sample_rate;
    int dst_rate = frame->sample_rate;
    
    m_dstNbSamples = av_rescale_rnd(swr_get_delay(m_swrCtx, src_rate) + frame->nb_samples, dst_rate, src_rate, AV_ROUND_UP);
    
    if(m_dstNbSamples > m_maxDstNbSamples)
    {
        av_free(m_dstData[0]);
        err = av_samples_alloc(m_dstData, &m_dstLinesize, m_dstNbChannels, m_dstNbSamples, AV_SAMPLE_FMT_S16, 1);
        m_maxDstNbSamples = m_dstNbSamples;
    }
    
    err = swr_convert(m_swrCtx, m_dstData, m_dstNbSamples, (const uint8_t**)frame->extended_data, frame->nb_samples);
    
    int dst_bufsize = av_samples_get_buffer_size(&m_dstLinesize, m_dstNbChannels, err, AV_SAMPLE_FMT_S16, 1);
    
    outNbSamples = dst_bufsize / av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
    outSamplesLength = dst_bufsize;
    outSamples = m_dstData[0];
}

bool MovieSound::onGetData(sf::SoundStream::Chunk &data)
{
    data.samples = m_samplesBuffer;
    
    while (data.sampleCount < av_get_channel_layout_nb_channels(AV_CH_LAYOUT_STEREO) * m_sampleRate)
    {
        bool needsMoreDecoding = false;
        bool gotFrame = false;
        
        std::unique_lock<std::mutex> lk(g_mut);
        g_newPktCondition.wait(lk, []{ return !g_audioPkts.empty();});
        
        AVPacket* packet = g_audioPkts.front();
        g_audioPkts.pop_front();
        lk.unlock();
        
        do {
            needsMoreDecoding = decodePacket(packet, m_audioFrame, gotFrame);
            
            if (gotFrame)
            {
                uint8_t* samples = NULL;
                int nbSamples = 0;
                int samplesLength = 0;
                
                resampleFrame(m_audioFrame, samples, nbSamples, samplesLength);
                
                std::memcpy((void*)(data.samples + data.sampleCount), samples, samplesLength);
                data.sampleCount += nbSamples;
            }
            
        }while (needsMoreDecoding);
        
        av_free_packet(packet);
        av_free(packet);
    }
    
    return true;
}

int main(int, char const**)
{
    AVFormatContext *pFormatCtx = NULL;
    AVCodecContext  *pCodecCtx = NULL;
    AVCodecContext  *paCodecCtx = NULL;
    AVCodec         *pCodec = NULL;
    AVCodec         *paCodec = NULL;
    AVFrame         *pFrame = NULL;
    AVFrame         *pFrameRGB = NULL;
    AVPacket        packet;
    int             frameFinished;
    int             numBytes;
    uint8_t         *buffer = NULL;
    
    AVDictionary    *optionsDict = NULL;
    AVDictionary    *optionsDictA = NULL;
    SwsContext      *sws_ctx = NULL;
    
    
    const char* filename = "/Users/JHQ/Desktop/McChrystal.mp4";
    //const char* filename = "/Users/JHQ/Downloads/bobb186.mp4/bobb186.mp4";
    // Register all formats and codecs
    av_register_all();
    
    // Open video file
    if(avformat_open_input(&pFormatCtx, filename, NULL, NULL)!=0)
        return -1; // Couldn't open file
    
    // Retrieve stream information
    if(avformat_find_stream_info(pFormatCtx, NULL)<0)
        return -1; // Couldn't find stream information
    
    // Dump information about file onto standard error
    av_dump_format(pFormatCtx, 0, filename, 0);
    
    int videoStream = -1;
    int audioStream = -1;
    for(int i = 0; i < pFormatCtx->nb_streams; ++i)
    {
        if(pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            videoStream = i;
        }
        else if(pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            audioStream = i;
        }
    }
    
    if(videoStream >= 0)
    {
        pCodecCtx = pFormatCtx->streams[videoStream]->codec;
        pCodec = avcodec_find_decoder(pCodecCtx->codec_id);
        
        if(avcodec_open2(pCodecCtx, pCodec, &optionsDict)<0)
            return -1;
    }
    if(audioStream >= 0)
    {
        paCodecCtx = pFormatCtx->streams[audioStream]->codec;
        paCodec = avcodec_find_decoder(paCodecCtx->codec_id);
        
        if(avcodec_open2(paCodecCtx, paCodec, &optionsDictA))
            return -1;
    }
 
    const int FrameSize = pCodecCtx->width * pCodecCtx->height * 3;
    
    pFrame = av_frame_alloc();
    pFrameRGB = av_frame_alloc();
    if (pFrameRGB == nullptr)
    {
        return -1;
    }
    
    numBytes = avpicture_get_size(PIX_FMT_RGB24, pCodecCtx->width, pCodecCtx->height);
    buffer = (uint8_t*)av_malloc(numBytes * sizeof(uint8_t));
    
    sws_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt, pCodecCtx->width, pCodecCtx->height, PIX_FMT_RGB24, SWS_BILINEAR, NULL, NULL, NULL);
    
    avpicture_fill((AVPicture*)pFrameRGB, buffer, PIX_FMT_RGB24, pCodecCtx->width, pCodecCtx->height);
    
    
    sf::Uint8* Data = new sf::Uint8[pCodecCtx->width * pCodecCtx->height * 4];
    
    // Create the main window
    sf::RenderWindow window(sf::VideoMode(pCodecCtx->width, pCodecCtx->height), "SFML window");

    sf::Texture im_video;
    im_video.create(pCodecCtx->width, pCodecCtx->height);
    im_video.setSmooth(false);
    
    // Set the Icon
    sf::Image icon;
    if (!icon.loadFromFile(resourcePath() + "icon.png")) {
        return EXIT_FAILURE;
    }
    window.setIcon(icon.getSize().x, icon.getSize().y, icon.getPixelsPtr());

    sf::Sprite sprite(im_video);

    // Create a graphical text to display
    sf::Font font;
    if (!font.loadFromFile(resourcePath() + "sansation.ttf")) {
        return EXIT_FAILURE;
    }
    sf::Text text("Hello SFML", font, 50);
    text.setColor(sf::Color::Black);
    
    MovieSound sound(pFormatCtx, audioStream);
    sound.play();


    // Start the game loop
    while (window.isOpen())
    {
        // Process events
        sf::Event event;
        while (window.pollEvent(event))
        {
            // Close window : exit
            if (event.type == sf::Event::Closed)
            {
                window.close();
            }

            // Escape pressed : exit
            if (event.type == sf::Event::KeyPressed && event.key.code == sf::Keyboard::Escape)
            {
                window.close();
            }
        }
        

        if(av_read_frame(pFormatCtx, &packet) < 0)
        {
            break;
        }
        
        if(packet.stream_index == videoStream)
        {
            avcodec_decode_video2(pCodecCtx, pFrame, &frameFinished, &packet);
            
            if(frameFinished)
            {
                sws_scale(sws_ctx, (uint8_t const * const *)pFrame->data, pFrame->linesize, 0, pCodecCtx->height, pFrameRGB->data, pFrameRGB->linesize);
                
                for (int i = 0, j = 0; i < FrameSize; i += 3, j += 4)
                {
                    Data[j + 0] = pFrameRGB->data[0][i + 0];
                    Data[j + 1] = pFrameRGB->data[0][i + 1];
                    Data[j + 2] = pFrameRGB->data[0][i + 2];
                    Data[j + 3] = 255;
                }
                
                im_video.update(Data);
                
                // Clear screen
                window.clear();
                
                window.draw(sprite);
                window.draw(text);
                
                window.display();
                
            }
        }
        else if(packet.stream_index == audioStream)
        {
            AVPacket* pkt = (AVPacket*)av_malloc(sizeof(AVPacket));
            std::memcpy(pkt, &packet, sizeof(AVPacket));
            
            std::lock_guard<std::mutex> lk(g_mut);
            g_audioPkts.push_back(pkt);
            g_newPktCondition.notify_one();
            
        }
        else
        {
            av_free_packet(&packet);
        }
        
    }
    
    av_free(buffer);
    av_free(pFrameRGB);
    av_free(pFrame);
    avcodec_close(pCodecCtx);
    avformat_close_input(&pFormatCtx);
    
    delete [] Data;

    return EXIT_SUCCESS;
}
