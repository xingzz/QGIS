/***************************************************************************
    qgsjsonutils.h
     -------------
    Date                 : May 206
    Copyright            : (C) 2016 Nyall Dawson
    Email                : nyall dot dawson at gmail dot com
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgsjsonutils.h"
#include "qgsfeatureiterator.h"
#include "qgsogrutils.h"
#include "qgsgeometry.h"
#include "qgsvectorlayer.h"
#include "qgsmaplayerregistry.h"
#include "qgsrelation.h"
#include "qgsrelationmanager.h"
#include "qgsproject.h"
#include "qgscsexception.h"
#include "qgslogger.h"

#include <QJsonDocument>
#include <QJsonArray>

QgsJSONExporter::QgsJSONExporter( const QgsVectorLayer* vectorLayer, int precision )
    : mPrecision( precision )
    , mIncludeGeometry( true )
    , mIncludeAttributes( true )
    , mIncludeRelatedAttributes( false )
    , mLayerId( vectorLayer ? vectorLayer->id() : QString() )
{
  if ( vectorLayer )
  {
    mCrs = vectorLayer->crs();
    mTransform.setSourceCrs( mCrs );
  }
  mTransform.setDestinationCrs( QgsCoordinateReferenceSystem( 4326, QgsCoordinateReferenceSystem::EpsgCrsId ) );
}

void QgsJSONExporter::setVectorLayer( const QgsVectorLayer* vectorLayer )
{
  mLayerId = vectorLayer ? vectorLayer->id() : QString();
  if ( vectorLayer )
  {
    mCrs = vectorLayer->crs();
    mTransform.setSourceCrs( mCrs );
  }
}

QgsVectorLayer *QgsJSONExporter::vectorLayer() const
{
  return qobject_cast< QgsVectorLayer* >( QgsMapLayerRegistry::instance()->mapLayer( mLayerId ) );
}

void QgsJSONExporter::setSourceCrs( const QgsCoordinateReferenceSystem& crs )
{
  mCrs = crs;
  mTransform.setSourceCrs( mCrs );
}

QgsCoordinateReferenceSystem QgsJSONExporter::sourceCrs() const
{
  return mCrs;
}

QString QgsJSONExporter::exportFeature( const QgsFeature& feature, const QVariantMap& extraProperties,
                                        const QVariant& id ) const
{
  QString s = "{\n   \"type\":\"Feature\",\n";

  // ID
  s += QString( "   \"id\":%1,\n" ).arg( !id.isValid() ? QString::number( feature.id() ) : QgsJSONUtils::encodeValue( id ) );

  QgsGeometry geom = feature.geometry();
  if ( !geom.isEmpty() && mIncludeGeometry )
  {
    if ( mCrs.isValid() )
    {
      try
      {
        QgsGeometry transformed = geom;
        if ( transformed.transform( mTransform ) == 0 )
          geom = transformed;
      }
      catch ( QgsCsException &cse )
      {
        Q_UNUSED( cse );
      }
    }
    QgsRectangle box = geom.boundingBox();

    if ( QgsWkbTypes::flatType( geom.geometry()->wkbType() ) != QgsWkbTypes::Point )
    {
      s += QString( "   \"bbox\":[%1, %2, %3, %4],\n" ).arg( qgsDoubleToString( box.xMinimum(), mPrecision ),
           qgsDoubleToString( box.yMinimum(), mPrecision ),
           qgsDoubleToString( box.xMaximum(), mPrecision ),
           qgsDoubleToString( box.yMaximum(), mPrecision ) );
    }
    s += "   \"geometry\":\n   ";
    s += geom.exportToGeoJSON( mPrecision );
    s += ",\n";
  }
  else
  {
    s += "   \"geometry\":null,\n";
  }

  // build up properties element
  QString properties;
  int attributeCounter = 0;
  if ( mIncludeAttributes || !extraProperties.isEmpty() )
  {
    //read all attribute values from the feature

    if ( mIncludeAttributes )
    {
      QgsFields fields = feature.fields();

      for ( int i = 0; i < fields.count(); ++i )
      {
        if (( !mAttributeIndexes.isEmpty() && !mAttributeIndexes.contains( i ) ) || mExcludedAttributeIndexes.contains( i ) )
          continue;

        if ( attributeCounter > 0 )
          properties += ",\n";
        QVariant val =  feature.attributes().at( i );

        properties += QString( "      \"%1\":%2" ).arg( fields.at( i ).name(), QgsJSONUtils::encodeValue( val ) );

        ++attributeCounter;
      }
    }

    if ( !extraProperties.isEmpty() )
    {
      QVariantMap::const_iterator it = extraProperties.constBegin();
      for ( ; it != extraProperties.constEnd(); ++it )
      {
        if ( attributeCounter > 0 )
          properties += ",\n";

        properties += QString( "      \"%1\":%2" ).arg( it.key(), QgsJSONUtils::encodeValue( it.value() ) );

        ++attributeCounter;
      }
    }

    // related attributes
    QgsVectorLayer* vl = vectorLayer();
    if ( vl && mIncludeRelatedAttributes )
    {
      QList< QgsRelation > relations = QgsProject::instance()->relationManager()->referencedRelations( vl );
      Q_FOREACH ( const QgsRelation& relation, relations )
      {
        if ( attributeCounter > 0 )
          properties += ",\n";

        QgsFeatureRequest req = relation.getRelatedFeaturesRequest( feature );
        req.setFlags( QgsFeatureRequest::NoGeometry );
        QgsVectorLayer* childLayer = relation.referencingLayer();
        QString relatedFeatureAttributes;
        if ( childLayer )
        {
          QgsFeatureIterator it = childLayer->getFeatures( req );
          QgsFeature relatedFet;
          int relationFeatures = 0;
          while ( it.nextFeature( relatedFet ) )
          {
            if ( relationFeatures > 0 )
              relatedFeatureAttributes += ",\n";

            relatedFeatureAttributes += QgsJSONUtils::exportAttributes( relatedFet );
            relationFeatures++;
          }
        }
        relatedFeatureAttributes.prepend( '[' ).append( ']' );

        properties += QString( "      \"%1\":%2" ).arg( relation.name(), relatedFeatureAttributes );
        attributeCounter++;
      }
    }
  }

  bool hasProperties = attributeCounter > 0;

  s += "   \"properties\":";
  if ( hasProperties )
  {
    //read all attribute values from the feature
    s += "{\n" + properties + "\n   }\n";
  }
  else
  {
    s += "null\n";
  }

  s += '}';

  return s;
}

QString QgsJSONExporter::exportFeatures( const QgsFeatureList& features ) const
{
  QStringList featureJSON;
  Q_FOREACH ( const QgsFeature& feature, features )
  {
    featureJSON << exportFeature( feature );
  }

  return QString( "{ \"type\": \"FeatureCollection\",\n    \"features\":[\n%1\n]}" ).arg( featureJSON.join( ",\n" ) );
}



//
// QgsJSONUtils
//

QgsFeatureList QgsJSONUtils::stringToFeatureList( const QString &string, const QgsFields &fields, QTextCodec *encoding )
{
  return QgsOgrUtils::stringToFeatureList( string, fields, encoding );
}

QgsFields QgsJSONUtils::stringToFields( const QString &string, QTextCodec *encoding )
{
  return QgsOgrUtils::stringToFields( string, encoding );
}

QString QgsJSONUtils::encodeValue( const QVariant &value )
{
  if ( value.isNull() )
    return "null";

  switch ( value.type() )
  {
    case QVariant::Int:
    case QVariant::UInt:
    case QVariant::LongLong:
    case QVariant::ULongLong:
    case QVariant::Double:
      return value.toString();

    case QVariant::Bool:
      return value.toBool() ? "true" : "false";

    case QVariant::StringList:
    case QVariant::List:
    case QVariant::Map:
      return QString::fromUtf8( QJsonDocument::fromVariant( value ).toJson( QJsonDocument::Compact ) );

    default:
    case QVariant::String:
      QString v = value.toString()
                  .replace( '\\', "\\\\" )
                  .replace( '"', "\\\"" )
                  .replace( '\r', "\\r" )
                  .replace( '\b', "\\b" )
                  .replace( '\t', "\\t" )
                  .replace( '/', "\\/" )
                  .replace( '\n', "\\n" );

      return v.prepend( '"' ).append( '"' );
  }
}

QString QgsJSONUtils::exportAttributes( const QgsFeature& feature )
{
  QgsFields fields = feature.fields();
  QString attrs;
  for ( int i = 0; i < fields.count(); ++i )
  {
    if ( i > 0 )
      attrs += ",\n";

    QVariant val = feature.attributes().at( i );
    attrs += encodeValue( fields.at( i ).name() ) + ':' + encodeValue( val );
  }
  return attrs.prepend( '{' ).append( '}' );
}

QVariantList QgsJSONUtils::parseArray( const QString& json, QVariant::Type type )
{
  QJsonParseError error;
  const QJsonDocument jsonDoc = QJsonDocument::fromJson( json.toUtf8(), &error );
  QVariantList result;
  if ( error.error != QJsonParseError::NoError )
  {
    QgsLogger::warning( QString( "Cannot parse json (%1): %2" ).arg( error.errorString(), json ) );
    return result;
  }
  if ( !jsonDoc.isArray() )
  {
    QgsLogger::warning( QString( "Cannot parse json (%1) as array: %2" ).arg( error.errorString(), json ) );
    return result;
  }
  Q_FOREACH ( const QJsonValue& cur, jsonDoc.array() )
  {
    QVariant curVariant = cur.toVariant();
    if ( curVariant.convert( type ) )
      result.append( curVariant );
    else
      QgsLogger::warning( QString( "Cannot convert json array element: %1" ).arg( cur.toString() ) );
  }
  return result;
}
