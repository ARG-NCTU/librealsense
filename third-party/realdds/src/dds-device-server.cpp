// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2022 Intel Corporation. All Rights Reserved.

#include <realdds/dds-device-server.h>

#include <realdds/dds-participant.h>
#include <realdds/dds-publisher.h>
#include <realdds/dds-subscriber.h>
#include <realdds/dds-stream-server.h>
#include <realdds/dds-stream-profile.h>
#include <realdds/dds-notification-server.h>
#include <realdds/dds-topic-reader.h>
#include <realdds/dds-device-broadcaster.h>
#include <realdds/dds-utilities.h>
#include <realdds/topics/dds-topic-names.h>
#include <realdds/topics/device-info-msg.h>
#include <realdds/topics/flexible-msg.h>
#include <realdds/dds-topic.h>
#include <realdds/dds-topic-writer.h>
#include <realdds/dds-option.h>
#include <realdds/dds-guid.h>

#include <fastdds/dds/subscriber/SampleInfo.hpp>

#include <rsutils/string/shorten-json-string.h>
#include <rsutils/json.h>
using rsutils::json;
using rsutils::string::slice;
using rsutils::string::shorten_json_string;

using namespace eprosima::fastdds::dds;
using namespace realdds;


static std::string const id_key( "id", 2 );
static std::string const id_set_option( "set-option", 10 );
static std::string const id_query_option( "query-option", 12 );
static std::string const value_key( "value", 5 );
static std::string const option_values_key( "option-values", 13 );
static std::string const sample_key( "sample", 6 );
static std::string const status_key( "status", 6 );
static std::string const status_ok( "ok", 2 );
static std::string const option_name_key( "option-name", 11 );
static std::string const stream_name_key( "stream-name", 11 );
static std::string const explanation_key( "explanation", 11 );
static std::string const control_key( "control", 7 );


dds_device_server::dds_device_server( std::shared_ptr< dds_participant > const & participant,
                                      const std::string & topic_root )
    : _publisher( std::make_shared< dds_publisher >( participant ) )
    , _subscriber( std::make_shared< dds_subscriber >( participant ) )
    , _topic_root( topic_root )
    , _control_dispatcher( QUEUE_MAX_SIZE )
{
    LOG_DEBUG( "device server created @ '" << _topic_root << "'" );
    _control_dispatcher.start();
}


dds_guid const & dds_device_server::guid() const
{
    return _notification_server ? _notification_server->guid() : unknown_guid;
}



dds_device_server::~dds_device_server()
{
    _stream_name_to_server.clear();
    LOG_DEBUG( "device server deleted @ '" << _topic_root << "'" );
}


static void on_discovery_device_header( size_t const n_streams,
                                        const dds_options & options,
                                        const extrinsics_map & extr,
                                        dds_notification_server & notifications )
{
    auto extrinsics_json = json::array();
    for( auto & ex : extr )
        extrinsics_json.push_back( json::array( { ex.first.first, ex.first.second, ex.second->to_json() } ) );

    topics::flexible_msg device_header( json{
        { id_key, "device-header" },
        { "n-streams", n_streams },
        { "extrinsics", std::move( extrinsics_json ) }
    } );
    auto json_string = slice( device_header.custom_data< char const >(), device_header._data.size() );
    LOG_DEBUG( "-----> JSON = " << shorten_json_string( json_string, 300 ) << " size " << json_string.length() );
    //LOG_DEBUG( "-----> CBOR size = " << json::to_cbor( device_header.json_data() ).size() );
    notifications.add_discovery_notification( std::move( device_header ) );

    auto device_options = rsutils::json::array();
    for( auto & opt : options )
        device_options.push_back( std::move( opt->to_json() ) );
    topics::flexible_msg device_options_message( json {
        { id_key, "device-options" },
        { "options", std::move( device_options ) }
    } );
    json_string = slice( device_options_message.custom_data< char const >(), device_options_message._data.size() );
    LOG_DEBUG( "-----> JSON = " << shorten_json_string( json_string, 300 ) << " size " << json_string.length() );
    //LOG_DEBUG( "-----> CBOR size = " << json::to_cbor( device_options_message.json_data() ).size() );
    notifications.add_discovery_notification( std::move( device_options_message ) );
}


static void on_discovery_stream_header( std::shared_ptr< dds_stream_server > const & stream,
                                        dds_notification_server & notifications )
{
    auto profiles = rsutils::json::array();
    for( auto & sp : stream->profiles() )
        profiles.push_back( std::move( sp->to_json() ) );
    topics::flexible_msg stream_header_message( json{
        { id_key, "stream-header" },
        { "type", stream->type_string() },
        { "name", stream->name() },
        { "sensor-name", stream->sensor_name() },
        { "profiles", std::move( profiles ) },
        { "default-profile-index", stream->default_profile_index() },
        { "metadata-enabled", stream->metadata_enabled() },
    } );
    auto json_string = slice( stream_header_message.custom_data< char const >(), stream_header_message._data.size() );
    LOG_DEBUG( "-----> JSON = " << shorten_json_string( json_string, 300 ) << " size " << json_string.length() );
    //LOG_DEBUG( "-----> CBOR size = " << json::to_cbor( stream_header_message.json_data() ).size() );
    notifications.add_discovery_notification( std::move( stream_header_message ) );

    auto stream_options = rsutils::json::array();
    for( auto & opt : stream->options() )
        stream_options.push_back( std::move( opt->to_json() ) );

    rsutils::json intrinsics;
    if( auto video_stream = std::dynamic_pointer_cast< dds_video_stream_server >( stream ) )
    {
        intrinsics = rsutils::json::array();
        for( auto & intr : video_stream->get_intrinsics() )
            intrinsics.push_back( intr.to_json() );
    }
    else if( auto motion_stream = std::dynamic_pointer_cast< dds_motion_stream_server >( stream ) )
    {
        intrinsics = rsutils::json::object( {
            { "accel", motion_stream->get_accel_intrinsics().to_json() },
            { "gyro", motion_stream->get_gyro_intrinsics().to_json() }
        } );
    }

    auto stream_filters = rsutils::json::array();
    for( auto & filter : stream->recommended_filters() )
        stream_filters.push_back( filter );
    topics::flexible_msg stream_options_message( json::object( {
        { id_key, "stream-options" },
        { "stream-name", stream->name() },
        { "options", std::move( stream_options ) },
        { "intrinsics", intrinsics },
        { "recommended-filters", std::move( stream_filters ) },
    } ) );
    json_string = slice( stream_options_message.custom_data< char const >(), stream_options_message._data.size() );
    LOG_DEBUG( "-----> JSON = " << shorten_json_string( json_string, 300 ) << " size " << json_string.length() );
    //LOG_DEBUG( "-----> CBOR size = " << json::to_cbor( stream_options_message.json_data() ).size() );
    notifications.add_discovery_notification( std::move( stream_options_message ) );
}


static std::string ros_friendly_topic_name( std::string name )
{
    // Replace all '/' with '_'
    int n = 0;
    for( char & ch : name )
        if( ch == '/' )
            if( ++n > 1 )
                ch = '_';
    // ROS topics start with "rt/" prefix
    name.insert( 0, "rt/", 3 );
    return name;
}


void dds_device_server::init( std::vector< std::shared_ptr< dds_stream_server > > const & streams,
                              const dds_options & options, const extrinsics_map & extr )
{
    if( is_valid() )
        DDS_THROW( runtime_error, "device server '" + _topic_root + "' is already initialized" );

    try
    {
        // Create a notifications server and set discovery notifications
        _notification_server = std::make_shared< dds_notification_server >( _publisher,
                                                                            _topic_root + topics::NOTIFICATION_TOPIC_NAME );

        // If a previous init failed (e.g., one of the streams has no profiles):
        _stream_name_to_server.clear();

        _options = options;
        on_discovery_device_header( streams.size(), options, extr, *_notification_server );
        for( auto & stream : streams )
        {
            std::string topic_name = ros_friendly_topic_name( _topic_root + '/' + stream->name() );
            stream->open( topic_name, _publisher );
            _stream_name_to_server[stream->name()] = stream;
            on_discovery_stream_header( stream, *_notification_server );

            if( stream->metadata_enabled() && ! _metadata_writer )
            {
                auto topic = topics::flexible_msg::create_topic( _publisher->get_participant(),
                                                                 _topic_root + topics::METADATA_TOPIC_NAME );
                _metadata_writer = std::make_shared< dds_topic_writer >( topic, _publisher );
                dds_topic_writer::qos wqos( eprosima::fastdds::dds::BEST_EFFORT_RELIABILITY_QOS );
                wqos.history().depth = 10;  // default is 1
                wqos.override_from_json( _subscriber->get_participant()->settings().nested( "device", "metadata" ) );
                _metadata_writer->run( wqos );
            }
        }

        _notification_server->run();

        // Create a control reader and set callback
        auto topic = topics::flexible_msg::create_topic( _subscriber->get_participant(), _topic_root + topics::CONTROL_TOPIC_NAME );
        _control_reader = std::make_shared< dds_topic_reader >( topic, _subscriber );

        _control_reader->on_data_available( [&]() { on_control_message_received(); } );

        dds_topic_reader::qos rqos( RELIABLE_RELIABILITY_QOS );
        rqos.override_from_json( _subscriber->get_participant()->settings().nested( "device", "control" ) );
        _control_reader->run( rqos );
    }
    catch( std::exception const & )
    {
        _notification_server.reset();
        _stream_name_to_server.clear();
        _control_reader.reset();
        throw;
    }
}


void dds_device_server::broadcast( topics::device_info const & device_info )
{
    if( _broadcaster )
        DDS_THROW( runtime_error, "device server was already broadcast" );
    if( ! _notification_server )
        DDS_THROW( runtime_error, "not initialized" );
    if( device_info.topic_root() != _topic_root )
        DDS_THROW( runtime_error, "device-info topic root does not match" );
    _broadcaster = std::make_shared< dds_device_broadcaster >(
        _publisher,
        device_info,
        [weak_notification_server = std::weak_ptr< dds_notification_server >( _notification_server )]
        {
            // Once we know our broadcast was acknowledged, send out discovery notifications again so any client who had
            // us marked offline can get ready again
            if( auto notification_server = weak_notification_server.lock() )
                notification_server->trigger_discovery_notifications();
        } );
}


void dds_device_server::broadcast_disconnect( dds_time ack_timeout )
{
    if( _broadcaster )
    {
        _broadcaster->broadcast_disconnect( ack_timeout );
        _broadcaster.reset();
    }
}


void dds_device_server::publish_notification( topics::flexible_msg && notification )
{
    _notification_server->send_notification( std::move( notification ) );
}


void dds_device_server::publish_metadata( rsutils::json && md )
{
    if( ! _metadata_writer )
        DDS_THROW( runtime_error, "device '" + _topic_root + "' has no stream with enabled metadata" );

    topics::flexible_msg msg( md );
    LOG_DEBUG(
        "publishing metadata: " << shorten_json_string( slice( msg.custom_data< char const >(), msg._data.size() ),
                                                        300 ) );
    std::move( msg ).write_to( *_metadata_writer );
}


bool dds_device_server::has_metadata_readers() const
{
    return _metadata_writer && _metadata_writer->has_readers();
}


void dds_device_server::on_control_message_received()
{
    topics::flexible_msg data;
    eprosima::fastdds::dds::SampleInfo info;
    while( topics::flexible_msg::take_next( *_control_reader, &data, &info ) )
    {
        if( ! data.is_valid() )
            continue;

        _control_dispatcher.invoke(
            [j = data.json_data(), sample = info, this]( dispatcher::cancellable_timer )
            {
                auto sample_j = json::array( {
                    rsutils::string::from( realdds::print_raw_guid( sample.sample_identity.writer_guid() ) ),
                    sample.sample_identity.sequence_number().to64long(),
                } );
                LOG_DEBUG( "<----- control " << sample_j << ": " << j );
                json reply;
                reply[sample_key] = std::move( sample_j );
                try
                {
                    std::string const & id = j.at( id_key ).string_ref();
                    reply[id_key] = id;
                    reply[control_key] = j;
                    handle_control_message( id, j, reply );
                }
                catch( std::exception const & e )
                {
                    reply[status_key] = "error";
                    reply[explanation_key] = e.what();
                }
                LOG_DEBUG( "----->   reply " << reply );
                try
                {
                    publish_notification( reply );
                }
                catch( ... )
                {
                    LOG_ERROR( "failed to send reply" );
                }
            } );
    }
}


void dds_device_server::handle_control_message( std::string const & id,
                                                rsutils::json const & j,
                                                rsutils::json & reply )
{
    if( id.compare( id_set_option ) == 0 )
    {
        handle_set_option( j, reply );
    }
    else if( id.compare( id_query_option ) == 0 )
    {
        handle_query_option( j, reply );
    }
    else if( ! _control_callback || ! _control_callback( id, j, reply ) )
    {
        DDS_THROW( runtime_error, "invalid control" );
    }
}


void dds_device_server::handle_set_option( const rsutils::json & j, rsutils::json & reply )
{
    auto & option_name = j.at( option_name_key ).string_ref();
    std::string stream_name;  // default is empty, for a device option
    j.nested( stream_name_key ).get_ex( stream_name );

    std::shared_ptr< dds_option > opt = find_option( option_name, stream_name );
    if( opt )
    {
        float value = j.at( value_key ).get< float >();
        if( _set_option_callback )
            _set_option_callback( opt, value ); //Handle setting option outside realdds
        opt->set_value( value ); //Update option object. Do second to check if _set_option_callback did not throw
        reply[value_key] = value;
    }
    else
    {
        if( stream_name.empty() )
            stream_name = "device";
        else
            stream_name = "'" + stream_name + "'";
        DDS_THROW( runtime_error, stream_name << " option '" << option_name << "' not found" );
    }
}


void dds_device_server::handle_query_option( const rsutils::json & j, rsutils::json & reply )
{
    std::string stream_name;  // default is empty, for a device option
    j.nested( stream_name_key ).get_ex( stream_name );

    auto query_option = [&]( std::shared_ptr< dds_option > const & option )
    {
        float value;
        if( _query_option_callback )
        {
            value = _query_option_callback( option );
            // Ensure realdds option is up to date with actual value from callback
            option->set_value( value );
        }
        else
        {
            value = option->get_value();
        }
        return value;
    };
    auto query_option_j = [&]( rsutils::json const & j )
    {
        if( ! j.is_string() )
            DDS_THROW( runtime_error, "option name should be a string; got " << j );
        std::string const & option_name = j.string_ref();
        std::shared_ptr< dds_option > option = find_option( option_name, stream_name );
        if( option )
            return query_option( option );

        if( stream_name.empty() )
            stream_name = "device";
        else
            stream_name = "'" + stream_name + "'";
        DDS_THROW( runtime_error, stream_name + " option '" + option_name + "' not found" );
    };

    auto option_name = j.nested( option_name_key );
    if( option_name.is_array() )
    {
        if( option_name.empty() )
        {
            // Query all options and return in option:value object
            rsutils::json & option_values = reply[option_values_key] = rsutils::json::object();
            if( stream_name.empty() )
            {
                for( auto const & option : _options )
                    option_values[option->get_name()] = query_option( option );
            }
            else
            {
                auto stream_it = _stream_name_to_server.find( stream_name );
                if( stream_it != _stream_name_to_server.end() )
                {
                    for( auto const & option : stream_it->second->options() )
                        option_values[option->get_name()] = query_option( option );
                }
            }
        }
        else
        {
            rsutils::json & value = reply[value_key];
            for( auto x = 0; x < option_name.size(); ++x )
                value.push_back( query_option_j( option_name.at( x ) ) );
        }
    }
    else
    {
        reply[value_key] = query_option_j( option_name );
    }
}


std::shared_ptr< dds_option > realdds::dds_device_server::find_option( const std::string & option_name,
                                                                       const std::string & stream_name ) const
{
    if( stream_name.empty() )
    {
        for( auto & option : _options )
        {
            if( option->get_name() == option_name )
                return option;
        }
    }
    else
    {
        // Find option in owner stream
        auto stream_it = _stream_name_to_server.find( stream_name );
        if( stream_it != _stream_name_to_server.end() )
        {
            for( auto & option : stream_it->second->options() )
            {
                if( option->get_name() == option_name )
                    return option;
            }
        }
    }

    return {};
}
